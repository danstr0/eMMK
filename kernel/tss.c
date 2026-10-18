/**
 * @file tss.c
 * @brief TSS and runtime management.
 *
 * Replaces the minimal boot GDT (3 entries from multiboot2.asm) with a
 * larger runtime GDT that includes per-core TSS descriptors. Each core
 * gets its own 104-byte TSS with IST stacks for double-fault and NMI
 * handling.
 */

#include "tss.h"
#include "smp.h"
#include "pmm.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

// ── Runtime GDT ────────────────────────────────────────────────────────

/**
 * The runtime GDT, large enough for the base descriptors plus one
 * 16-byte TSS descriptor per core. Aligned to 16 bytes as
 * recommended by Intel.
 */
static uint64_t runtime_gdt[GDT_MAX_ENTRIES] ALIGNED(16);

/// LGDT operand for the runtime GDT.
static struct
{
    uint16_t limit;
    uint64_t base;
} PACKED runtime_gdt_ptr;

// ── Per-core TSS storage ───────────────────────────────────────────────

static struct tss per_core_tss[SMP_MAX_CORES] ALIGNED(16);

// ── GDT helpers ────────────────────────────────────────────────────────

/**
 * @brief Write a 64-bit TSS descriptor into the GDT.
 *
 * A TSS descriptor in long mode is 16 bytes (two consecutive GDT
 * slots): the low 8 bytes contain the segment limit, base[23:0],
 * type, and base[31:24]; the high 8 bytes contain base[63:32].
 *
 * @param slot      GDT slot index (first of the two slots consumed).
 * @param tss_addr  Virtual address of the TSS.
 * @param tss_size  Size of the TSS in bytes (typically 104).
 */
static void gdt_write_tss_descriptor(int slot, uint64_t tss_addr,
                                     uint32_t tss_size)
{
    uint32_t limit = tss_size - 1;
    uint16_t base_lo  = (uint16_t)(tss_addr & 0xFFFF);
    uint8_t  base_mid = (uint8_t)((tss_addr >> 16) & 0xFF);
    uint8_t  base_hi  = (uint8_t)((tss_addr >> 24) & 0xFF);
    uint32_t base_upper = (uint32_t)(tss_addr >> 32);

    // Low 8 bytes:
    //   [15:0]   limit[15:0]
    //   [31:16]  base[15:0]
    //   [39:32]  base[23:16]
    //   [43:40]  type (0x9 = 64-bit TSS available)
    //   [44]     0 (system descriptor)
    //   [46:45]  DPL = 0
    //   [47]     present = 1
    //   [51:48]  limit[19:16]
    //   [55:52]  flags (G=0, 0, 0, AVL=0)
    //   [63:56]  base[31:24]
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);             // limit[15:0]
    low |= (uint64_t)base_lo << 16;                // base[15:0]
    low |= (uint64_t)base_mid << 32;               // base[23:16]
    low |= (uint64_t)0x89 << 40;                   // type=9, P=1
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;  // limit[19:16]
    low |= (uint64_t)base_hi << 56;                // base[31:24]

    // High 8 bytes: base[63:32] and reserved
    uint64_t high = (uint64_t)base_upper;

    runtime_gdt[slot]     = low;
    runtime_gdt[slot + 1] = high;
}

// ── IST stack allocation ───────────────────────────────────────────────

/**
 * @brief Allocate a stack for an IST entry.
 * @return Virtual address of the stack top, or @c 0 on failure.
 */
static uint64_t alloc_ist_stack(void)
{
    size_t pages = IST_STACK_SIZE / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == PMM_ALLOC_FAILED)
        return 0;
    return (uint64_t)PHYS_TO_VIRT(phys) + IST_STACK_SIZE;
}

// ── Per-core TSS setup ─────────────────────────────────────────────────

/**
 * @brief Initialize a TSS for a given core.
 *
 * Zeroes the TSS, allocates IST stacks for double-fault and NMI,
 * sets RSP0, and writes the TSS descriptor into the runtime GDT.
 */
static void setup_core_tss(int core_index, uint64_t kernel_stack_top)
{
    struct tss *t = &per_core_tss[core_index];
    memset(t, 0, sizeof(*t));

    // RSP0: used if an interrupt is ever taken from a lower ring
    //   - not applicable in ring-0-only eMMK, but must be valid
    t->rsp0 = kernel_stack_top;

    // IST stacks for critical exceptions
    t->ist1 = alloc_ist_stack();  // IST_DOUBLE_FAULT
    t->ist2 = alloc_ist_stack();  // IST_NMI

    if (t->ist1 == 0 || t->ist2 == 0)
        kprintf("[TSS] WARNING: Failed to allocate IST stack for core %d\n",
                core_index);

    // I/O permission bitmap not used - point past the TSS
    t->iopb_offset = sizeof(struct tss);

    // Write the TSS descriptor into the GDT
    int gdt_slot = GDT_BASE_ENTRIES + core_index * GDT_TSS_SLOTS;
    gdt_write_tss_descriptor(gdt_slot, (uint64_t)t, sizeof(struct tss));
}

/// Load the Task Register for the current core.
static inline void load_tr(int core_index)
{
    uint16_t selector = TSS_SELECTOR(core_index);
    __asm__ volatile("ltr %0" : : "r"(selector));
}

// ── Public API ─────────────────────────────────────────────────────────

void tss_init(void)
{
    kprintf("[TSS] Building runtime GDT...\n");

    // Copy the boot GDT's base entries (null, code, data)
    extern uint64_t gdt64[];  // from multiboot2.asm

    memset(runtime_gdt, 0, sizeof(runtime_gdt));
    runtime_gdt[0] = gdt64[0];  // null
    runtime_gdt[1] = gdt64[1];  // 64-bit code
    runtime_gdt[2] = gdt64[2];  // 64-bit data

    // Set up the BSP's TSS (core 0)
    // stack_top (entry64.asm) is a physical-address symbol; the BSP runs
    // on its higher-half alias
    extern char stack_top[];
    setup_core_tss(0, (uint64_t)stack_top + KERNEL_VIRTUAL_BASE);

    // Build and load the new GDT
    int total_slots = GDT_BASE_ENTRIES + SMP_MAX_CORES * GDT_TSS_SLOTS;
    runtime_gdt_ptr.limit = (uint16_t)(total_slots * 8 - 1);
    runtime_gdt_ptr.base  = (uint64_t)runtime_gdt;

    __asm__ volatile("lgdt %0" : : "m"(runtime_gdt_ptr));

    // Reload code and data segments after switching GDTs.
    // The selectors (0x08, 0x10) are the same as the boot GDT.
    __asm__ volatile(
            "pushq $0x08\n"
            "leaq 1f(%%rip), %%rax\n"
            "pushq %%rax\n"
            "lretq\n"
            "1:\n"
            "mov $0x10, %%ax\n"
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            "mov %%ax, %%fs\n"
            "mov %%ax, %%gs\n"
            "mov %%ax, %%ss\n"
            ::: "rax", "memory"
    );

    // Load the BSP's Task Register
    load_tr(0);

    kprintf("[TSS] Runtime GDT loaded: %d slots, BSP TR=0x%x\n",
            total_slots, TSS_SELECTOR(0));
    kprintf("[TSS] BSP IST1(#DF)=%p, IST2(NMI)=%p\n",
            (void *)per_core_tss[0].ist1,
            (void *)per_core_tss[0].ist2);
}

void tss_init_ap(int core_index, uint64_t stack_top)
{
    if (core_index < 0 || core_index >= SMP_MAX_CORES)
        return;

    // The stack is passed in rather than looked up via smp_get_core():
    // a starting AP is not yet counted in core_count, so that lookup
    // returns NULL here.
    setup_core_tss(core_index, stack_top);

    // Load the runtime GDT - the AP is still using the boot GDT
    // (which has no TSS descriptors)
    __asm__ volatile("lgdt %0" : : "m"(runtime_gdt_ptr));

    load_tr(core_index);

    kprintf("[TSS] Core %d: TR=0x%x, IST1=%p, IST2=%p\n",
            core_index, TSS_SELECTOR(core_index),
            (void *)per_core_tss[core_index].ist1,
            (void *)per_core_tss[core_index].ist2);
}

struct tss *tss_get(int core_index)
{
    if (core_index < 0 || core_index >= SMP_MAX_CORES)
        return NULL;
    return &per_core_tss[core_index];
}

