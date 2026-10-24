/**
 * @file smp.c
 * @brief SMP initialization — AP discovery and startup.
 *
 * @par Core discovery
 * The number of logical processors is taken from CPUID leaf 1
 * (EBX[23:16]) and APIC IDs are assumed to be 0, 1, 2, ... (true on
 * QEMU). TODO: revisit with real-hardware update.
 *
 * @par Core indices
 * Core indices are dense: 0 is the BSP, and each AP that starts
 * successfully gets the next index. An AP that fails to report in is sent
 * an INIT IPI (parking it in wait-for-SIPI) so that it cannot wake up
 * later on an index that has been given to another core.
 */

#include "smp.h"
#include "apic.h"
#include "cpu.h"
#include "idt.h"
#include "tss.h"
#include "percpu.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "tsc.h"

// ── Trampoline symbols (from ap_trampoline.asm) ────────────────────────

// Physical-address symbols (VMA == LMA); access through the kernel image
// mapping (+ KERNEL_VIRTUAL_BASE)
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

// ── GDT symbols (from multiboot2.asm, used by APs) ─────────────────────

extern uint8_t gdt64_pointer[];  // 10-byte LGDT operand (physical symbol)

// ── Global state ───────────────────────────────────────────────────────

static struct core_info cores[SMP_MAX_CORES];
static int              core_count = 0;

/// The parameter block in the trampoline page (virtual address).
static volatile struct ap_boot_params *ap_params;

/// Kernel-image alias of a physical-address boot symbol.
#define IMAGE_ALIAS(sym)  ((void *)((uintptr_t)(sym) + KERNEL_VIRTUAL_BASE))

// ── Core identification ────────────────────────────────────────────────

int smp_get_core_count(void)
{
    return core_count;
}

struct core_info *smp_get_core(int index)
{
    if (index < 0 || index >= core_count)
        return NULL;
    return &cores[index];
}

// ── Trampoline setup ───────────────────────────────────────────────────

/**
 * @brief Copy the trampoline and write the shared boot parameters.
 *
 * The trampoline code is position-dependent on @c AP_TRAMPOLINE_ADDR.
 * The parameter block is at offset @c AP_PARAMS_OFFSET within the
 * same page.
 */
static void setup_trampoline(void)
{
    size_t tramp_size = (size_t)(ap_trampoline_end - ap_trampoline_start);

    // Write the trampoline page through the direct map
    uint8_t *dest = (uint8_t *)PHYS_TO_VIRT(AP_TRAMPOLINE_ADDR);

    // Zero the whole page first, then copy the trampoline code
    memset(dest, 0, PAGE_SIZE);
    memcpy(dest, IMAGE_ALIAS(ap_trampoline_start), tramp_size);

    // Set up the parameter block pointer
    ap_params = (volatile struct ap_boot_params *)
        (dest + AP_PARAMS_OFFSET);

    // PML4: read from the BSP's CR3
    ap_params->pml4_addr = (uint32_t)vmm_get_pml4();

    // GDT: the boot GDT from multiboot2.asm (null, code, data). It sbase
    // is a physical address, reachable through the identity map while
    // the AP is in compatibility mode. ap_entry switches to the runtime
    // GDT (tss_init_ap) before the identity map is removed.
    const uint8_t *gdt_ptr = IMAGE_ALIAS(gdt64_pointer);
    uint16_t gdt_limit = *(const uint16_t *)gdt_ptr;
    uint64_t gdt_base  = *(const uint64_t *)(gdt_ptr + 2);

    ap_params->gdt_limit    = gdt_limit;
    ap_params->gdt_ptr_low  = (uint32_t)(gdt_base & 0xFFFFFFFF);
    ap_params->gdt_ptr_high = (uint32_t)(gdt_base >> 32);

    ap_params->entry_point = (uint64_t)ap_entry;

    kprintf("[SMP] Trampoline at phys 0x%lx, size %zu bytes\n",
            (uint64_t)AP_TRAMPOLINE_ADDR, tramp_size);
}

// ── AP startup (INIT-SIPI-SIPI) ────────────────────────────────────────

/// Send an INIT IPI to @p apic_id (resets it into wait-for-SIPI).
static void send_init(uint32_t apic_id)
{
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_ASSERT);
}

/**
 * @brief Start a single AP.
 *
 * @param apic_id     Target AP's Local APIC ID.
 * @param core_index  Core index the AP will take.
 * @param stack       Virtual address of the AP's stack top.
 * @return @c true if the AP reported ready within the timeout.
 */
static bool start_ap(uint32_t apic_id, int core_index, uint64_t stack)
{
    // Write per-AP parameters
    ap_params->stack_top  = stack;
    ap_params->ap_ready   = 0;
    ap_params->ap_id      = apic_id;
    ap_params->core_index = (uint32_t)core_index;

    // Ensure writes are visible before sending IPIs
    barrier();

    // INIT IPI: resets the AP to its power-on state.
    // Intel SDM Vol. 3, §11.4.7.3.
    send_init(apic_id);

    timer_sleep_ms(10);

    // SIPI: tells the AP to begin execution at the trampoline's
    // physical page number. The vector field of the SIPI encodes
    // the page number (0x08 -> physical 0x8000).
    // Send twice per the Intel recommendation (§9.4.4).
    uint8_t vector = (uint8_t)(AP_TRAMPOLINE_ADDR >> 12);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
        lapic_write(LAPIC_ICR_LOW,
                    LAPIC_ICR_STARTUP | vector
                    | LAPIC_ICR_LEVEL_ASSERT);
        tsc_delay_us(200);
    }

    // Wait for the AP to signal ready (up to 100 ms)
    uint64_t deadline = timer_deadline_ms(100);
    while (!ap_params->ap_ready)
    {
        if (timer_deadline_expired(deadline))
        {
            // Park it: a late AP must not run on an index that the next
            // AP is about to be given.
            send_init(apic_id);
            kprintf("[SMP] AP %lu failed to start (timeout); parked\n",
                    (uint64_t)apic_id);
            return false;
        }
        cpu_relax();
    }

    return true;
}

// ── AP C entry point ───────────────────────────────────────────────────

NORETURN void ap_entry(void)
{
    // At this point:
    // - In 64-bit long mode on the per-AP stack
    // - CR3 points to the BSP's page tables
    // - The boot GDT is loaded (via the trampoline); no IDT, TSS, LAPIC
    //   setup, or per-CPU area yet
    //
    // The BSP waits for ap_ready before touching ap_params again, so the
    // parameters below are stable

    // Control registers first, then the per-CPU area, so that
    // this_core_id() is valid for everything that follows
    cpu_init_core();

    int      my_core = (int)ap_params->core_index;
    uint32_t my_id = ap_params->ap_id;
    uint64_t my_stack = ap_params->stack_top;
    percpu_init(my_core, my_id);

    // Reload segment registers
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%ss\n"
        ::: "ax"
    );

    // Load the IDT (same table as the BSP)
    extern struct idt_entry idt[];
    struct
    {
        uint16_t limit;
        uint64_t base;
    } PACKED idtr =
    {
        .limit = 256 * 16 - 1,
        .base  = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr));

    // Enable this AP's Local APIC
    lapic_write(LAPIC_SVR, APIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);
    lapic_write(LAPIC_TPR, 0);

    // Clear any stale errors and pending interrupts
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_eoi();

    // Install per-core TSS (IST stacks) and switch to the runtime GDT
    tss_init_ap(my_core, my_stack);

    // Enable interrupts on this core
    __asm__ volatile("sti");

    kprintf("[SMP] AP %lu online (core %d)\n", (uint64_t)my_id, my_core);

    // Signal the BSP that we're ready
    cores[my_core].online = true;
    barrier();
    ap_params->ap_ready = 1;

    // Wait for the BSP to assign a role and launch
    __asm__ volatile("cli");
    while (!cores[my_core].launched)
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    __asm__ volatile("sti");

    // The BSP removed the identity map after smp_init(); drop any
    // translations this core still caches from the trampoline
    vmm_flush_tlb_all();

    if (cores[my_core].run_func)
    {
        kprintf("[SMP] Core %d entering role: %s\n",
                my_core,
                cores[my_core].role_name
                    ? cores[my_core].role_name
                    : "unnamed");
        cores[my_core].run_func(my_core);
    }

    // No function assigned, or function returned
    for (;;)
        __asm__ volatile("sti; hlt");
}

// ── BSP initialization ─────────────────────────────────────────────────

int smp_init(void)
{
    kprintf("[SMP] Initializing multi-core support...\n");

    // Register the BSP as core 0
    memset(cores, 0, sizeof(cores));

    cores[0].apic_id = lapic_id();
    cores[0].online  = true;
    cores[0].is_bsp  = true;
    core_count = 1;
    percpu_set_apic_id(cores[0].apic_id);

    kprintf("[SMP] BSP: APIC ID %lu\n", (uint64_t)cores[0].apic_id);

    // Determine how many APs exist. TODO: ACPI MADT
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    int max_cores = (ebx >> 16) & 0xFF;
    if (max_cores < 1)
        max_cores = 1;
    if (max_cores > SMP_MAX_CORES)
        max_cores = SMP_MAX_CORES;

    kprintf("[SMP] CPUID reports up to %d logical processors\n", max_cores);

    if (max_cores <= 1)
    {
        kprintf("[SMP] Single-core system - no APs to start\n");
        vmm_unmap_identity();
        return 0;
    }

    // Prepare the trampoline
    setup_trampoline();

    // Start each AP
    int aps_started = 0;
    for (int i = 1; i < max_cores; ++i)
    {
        uint32_t ap_apic_id = (uint32_t)i;  // TODO: from the MADT
        int      index      = core_count;

        // Allocate a guarded stack for this AP
        uint64_t stack = vmm_alloc_stack(AP_STACK_SIZE);
        if (stack == 0)
        {
            kprintf("[SMP] Failed to allocate stack for AP %d\n", i);
            continue;
        }

        // Pre-register the core so ap_entry() can find itself
        memset(&cores[index], 0, sizeof(cores[index]));
        cores[index].apic_id    = ap_apic_id;
        cores[index].stack_base = (void *)(stack - AP_STACK_SIZE);

        if (start_ap(ap_apic_id, index, stack))
        {
            core_count++;
            aps_started++;
            kprintf("[SMP] AP %lu started successfully\n",
                    (uint64_t)ap_apic_id);
        }
        else
            memset(&cores[index], 0, sizeof(cores[index]));
    }

    kprintf("[SMP] %d AP(s) started, %d total core(s) online\n",
            aps_started, core_count);

    // Every AP is now past the trampoline and on the runtime GDT
    vmm_unmap_identity();

    return aps_started;
}

// ── Per-core work assignment ───────────────────────────────────────────

void smp_assign_role(int core_index, core_run_func_t func,
                     const char *name)
{
    if (core_index < 0 || core_index >= core_count)
        return;
    cores[core_index].run_func  = func;
    cores[core_index].role_name = name;
    kprintf("[SMP] Core %d assigned role: %s\n", core_index, name);
}

void smp_launch_all(void)
{
    kprintf("[SMP] Launching all cores...\n");
    barrier();
    for (int i = 1; i < core_count; ++i)
        if (cores[i].online)
        {
            cores[i].launched = true;

            // Wake the AP from HLT
            lapic_send_ipi(cores[i].apic_id, APIC_WAKE_VECTOR);
        }
    barrier();
}

const char *smp_get_role_name(int core_index)
{
    if (core_index < 0 || core_index >= core_count)
        return "unknown";
    return cores[core_index].role_name
         ? cores[core_index].role_name
         : "idle";
}

