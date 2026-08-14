/**
 * @file smp.c
 * @brief SMP initialization — AP discovery and startup.
 *
 * @par Core discovery
 * For QEMU, the number of cores is detected by sending a broadcast
 * SIPI and counting responses. On real hardware this would come from
 * the ACPI MADT.
 *
 * @par APIC ID → core index mapping
 * QEMU assigns sequential APIC IDs starting from 0. The BSP is always
 * APIC ID 0 = core index 0. This assumption is documented and sufficient
 * for the QEMU target.
 */

#include "smp.h"
#include "apic.h"
#include "idt.h"
#include "tss.h"
#include "percpu.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "tsc.h"

// ── Trampoline symbols (from ap_trampoline.asm) ────────────────────────

extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

// ── GDT symbols (from multiboot2.asm, used by APs) ─────────────────────

extern uint8_t gdt64[];
extern uint8_t gdt64_pointer[];  // 10-byte LGDT operand

// ── Global state ───────────────────────────────────────────────────────

static struct core_info cores[SMP_MAX_CORES];
static int              core_count = 0;

/// The parameter block in the trampoline page (virtual address).
static volatile struct ap_boot_params *ap_params;

// ── Core identification ────────────────────────────────────────────────

int smp_get_core_count(void)
{
    return core_count;
}

int smp_core_id(void)
{
    // Fast path: if the per-CPU area has been initialized (GS base is
    // set), read the core index directly from %gs. This is a single
    // mov instruction with no LAPIC read or loop.
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_GS_BASE));
    uint64_t gs_base = ((uint64_t)hi << 32) | lo;

    if (gs_base != 0)
        return this_core_id();

    // Before SMP init, only BSP is running
    if (core_count == 0)
        return 0;

    // Fallback: scan by APIC ID
    uint32_t id = lapic_id();
    for (int i = 0; i < core_count; ++i)
        if (cores[i].apic_id == id)
            return i;

    return 0;
}

struct core_info *smp_get_core(int index)
{
    if (index < 0 || index >= core_count)
        return NULL;
    return &cores[index];
}

// ── Per-AP stack allocation ────────────────────────────────────────────

/**
 * @brief Allocate a stack for an AP.
 * @return Virtual address of the stack top, or @c 0 on failure.
 */
static uint64_t alloc_ap_stack(void)
{
    size_t pages = AP_STACK_SIZE / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == PMM_ALLOC_FAILED)
        return 0;
    return (uint64_t)PHYS_TO_VIRT(phys) + AP_STACK_SIZE;
}

// ── Trampoline setup ───────────────────────────────────────────────────

/**
 * @brief Copy the trampoline and write boot parameters.
 *
 * The trampoline code is position-dependent on @c AP_TRAMPOLINE_ADDR.
 * The parameter lock is at offset @c AP_PARAMS_OFFSET within the
 * same page.
 */
static void setup_trampoline(void)
{
    size_t tramp_size = (size_t)(ap_trampoline_end - ap_trampoline_start);

    // The trampoline physical address is identity-mapped
    uint8_t *dest = (uint8_t *)PHYS_TO_VIRT(AP_TRAMPOLINE_ADDR);

    // Zero the whole page first, then copy the trampoline code
    memset(dest, 0, PAGE_SIZE);
    memcpy(dest, ap_trampoline_start, tramp_size);

    // Set up the parameter block pointer
    ap_params = (volatile struct ap_boot_params *)
        (dest + AP_PARAMS_OFFSET);

    // PML4: read from the BSP's CR3
    ap_params->pml4_addr = (uint32_t)vmm_get_pml4();

    // GDT: the 64-bit GDT defined in multiboot2.asm. The APs need
    // both the base address and the limit. The BSP has already loaded
    // this GDT; extract the values from the LGDT operand.
    uint16_t *gdt_ptr_limit = (uint16_t *)gdt64_pointer;
    uint64_t *gdt_ptr_base  = (uint64_t *)(gdt64_pointer + 2);

    ap_params->gdt_limit    = *gdt_ptr_limit;
    ap_params->gdt_ptr_low  = (uint32_t)(*gdt_ptr_base & 0xFFFFFFFF);
    ap_params->gdt_ptr_high = (uint32_t)(*gdt_ptr_base >> 32);

    ap_params->entry_point = (uint64_t)ap_entry;

    kprintf("[SMP] Trampoline at phys 0x%lx, size %lu bytes\n",
            (uint64_t)AP_TRAMPOLINE_ADDR, (uint64_t)tramp_size);
}

// ── AP startup (INIT-SIPI-SIPI) ────────────────────────────────────────

/**
 * @brief Start a single AP.
 *
 * @param apic_id  Target AP's Local APIC ID.
 * @param stack    Virtual address of the AP's stack top.
 * @return @c true if the AP reported ready within the timeout.
 */
static bool start_ap(uint32_t apic_id, uint64_t stack)
{
    // Write per-AP parameters
    ap_params->stack_top = stack;
    ap_params->ap_ready  = 0;
    ap_params->ap_id     = apic_id;

    // Ensure writes are visible before sending IPIs
    barrier();

    // INIT IPI: resets the AP to its power-on state.
    // Intel SDM Vol. 3, §11.4.7.3.
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW,
                LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_ASSERT);

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
            kprintf("[SMP] AP %lu failed to start (timeout)\n",
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
    // - In 64-bit long mode with a fresh per-AP stack
    // - CR3 points to the BSP's page tables
    // - The BSP's runtime GDT is loaded (via the trampoline)
    // - No LAPIC enabled yet, no TSS, no per-CPU area

    // Reload segment registers
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%ss\n"
        ::: "ax"
    );
    // GS is set later by percpu_init_ap via wrmsr

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

    // Read our APIC ID and find our core index
    uint32_t my_id = lapic_id();
    int my_core = -1;
    for (int i = 0; i < SMP_MAX_CORES; ++i)
        if (cores[i].apic_id == my_id)
        {
            my_core = i;
            break;
        }

    // Install per-core TSS and load the Task Register
    if (my_core >= 0)
        tss_init_ap(my_core);

    // Set up the per-CPU area (sets GS base)
    if (my_core >= 0)
        percpu_init_ap(my_core, my_id);

    // Enable interrupts on this core
    __asm__ volatile("sti");

    kprintf("[SMP] AP %lu online (core %d)\n",
            (uint64_t)my_id, my_core);

    // Signal the BSP that we're ready
    if (my_core >= 0)
        cores[my_core].online = true;
    ap_params->ap_ready = 1;
    barrier();

    // Wait for the BSP to assign a role and launch
    while (!cores[my_core].launched)
        __asm__ volatile("sti;hlt");

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

    kprintf("[SMP] BSP: APIC ID %lu\n", (uint64_t)cores[0].apic_id);

    // Determine how many APs exist
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    int max_cores = (ebx >> 16) & 0xFF;
    if (max_cores < 1)
        max_cores = 1;
    if (max_cores > SMP_MAX_CORES)
        max_cores = SMP_MAX_CORES;

    kprintf("[SMP] CPUID reports up to %d logical processors\n", max_cores);

    if (max_cores <= 1)
    {
        kprintf("[SMP] Single-core system - no APs to start\n");
        return 0;
    }

    // Prepare the trampoline
    setup_trampoline();

    // Start each AP
    int aps_started = 0;
    for (int i = 1; i < max_cores; ++i)
    {
        uint32_t ap_apic_id = (uint32_t)i;  // QEMU: sequential IDs

        // Allocate a stack for this AP
        uint64_t stack = alloc_ap_stack();
        if (stack == 0)
        {
            kprintf("[SMP] Failed to allocate stack for AP %d\n", i);
            continue;
        }

        // Pre-register the core so ap_entry() can find itself
        cores[i].apic_id    = ap_apic_id;
        cores[i].online     = false;
        cores[i].is_bsp     = false;
        cores[i].stack_base = (void *)(stack - AP_STACK_SIZE);

        if (start_ap(ap_apic_id, stack))
        {
            core_count++;
            aps_started++;
            kprintf("[SMP] AP %lu started successfully\n",
                    (uint64_t)ap_apic_id);
        }
    }

    kprintf("[SMP] %d AP(s) started, %d total core(s) online\n",
            aps_started, core_count);

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

            // Send a fixed vector IPI or direct interrupt to wake AP from HLT
            lapic_write(LAPIC_ICR_HIGH, cores[i].apic_id << 24);
            lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_LEVEL_ASSERT | APIC_SPURIOUS_VECTOR);
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

