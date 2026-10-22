/**
 * @file vmm.c
 * @brief Virtual Memory Manager - 4-level page table operations.
 *
 * @par Bootstrapping
 * @c vmm_init() runs while the boot page tables from multiboot2.asm are
 * still active. Those map only the first 1 GiB, so every page table built
 * during init is allocated below 1 GiB and accessed through the boot
 * window (@c pmm_phys_to_virt handles this). After CR3 is switched, all
 * page-table access goes through the direct map.
 */

#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "serial.h"
#include "string.h"
#include "spinlock.h"

// ── Linker symbols ─────────────────────────────────────────────────────

// Boot sections: values are physical addresses
extern char __boot_start[], __boot_end[];
extern char __boot_stack_start[], __boot_stack_end[];
extern char _ap_trampoline_lma[], __ap_trampoline_end[];

// Higher-half sections: values are virtual addresses
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], _kernel_virtual_end[];

// ── State ──────────────────────────────────────────────────────────────

/// Virtual pointer to the kernel's top-level page table (PML4).
static pte_t *kernel_pml4;

/// Serializes all page-table modifications and the MMIO window cursor.
static spinlock_t vmm_lock = SPINLOCK_INIT;

/// CPU supports 1 GiB pages.
static bool has_1g_pages;

/// Next free address in the MMIO window.
static uint64_t mmio_next = MMIO_VIRTUAL_BASE;

/// Page counts by size, for the direct-map summary.
static size_t map_count_4k, map_count_2m, map_count_1g;

// ── Internal helpers ───────────────────────────────────────────────────

/// Extract the physical address from a PTE and convert to a virtual pointer.
static inline pte_t *pte_to_virt(pte_t entry)
{
    return (pte_t *)pmm_phys_to_virt(entry & PTE_ADDR_MASK);
}

/**
 * @brief Walk (or create) one level of the page table hierarchy.
 *
 * @param table     Current-level table (virtual pointer).
 * @param index     Index within @p table.
 * @param create    If @c true, allocate a new table when the entry is
 *                  not present.
 * @param[out] err  Set to the reason when @c NULL is returned.
 *
 * @return Virtual pointer to the next-level table, or @c NULL if:
 *         - the entry is not present and @p create is @c false
 *           (@c VMM_ERR_NOT_MAPPED),
 *         - the entry is a huge page (@c VMM_ERR_INVALID), or
 *         - allocation failed (@c VMM_ERR_NO_MEMORY).
 */
static pte_t *next_level(pte_t *table, size_t index, bool create, int *err)
{
    pte_t *entry = &table[index];

    if (!(*entry & PTE_PRESENT))
    {
        if (!create)
        {
            *err = VMM_ERR_NOT_MAPPED;
            return NULL;
        }

        uint64_t paddr = pmm_alloc_page_zero();
        if (paddr == PMM_ALLOC_FAILED)
        {
            kprintf("[VMM] ERROR: Failed to allocate page table frame\n");
            *err = VMM_ERR_NO_MEMORY;
            return NULL;
        }

        *entry = paddr | PTE_PRESENT | PTE_WRITABLE;
    }

    if (*entry & PTE_HUGE)
    {
        *err = VMM_ERR_INVALID;
        return NULL;
    }

    return pte_to_virt(*entry);
}

/**
 * @brief Install one leaf mapping of @p page_size bytes.
 *
 * Caller holds @c vmm_lock.
 *
 * @param page_size  @c PAGE_SIZE, @c PAGE_SIZE_2M, or @c PAGE_SIZE_1G;
 *                   @p vaddr and @p paddr must be aligned to it.
 */
static int map_one_locked(uint64_t vaddr, uint64_t paddr, uint64_t flags,
                          uint64_t page_size)
{
    int err = VMM_OK;
    pte_t *pdpt = next_level(kernel_pml4, VMM_PML4_INDEX(vaddr), true, &err);
    if (!pdpt)
        return err;

    pte_t *leaf_table;
    size_t leaf_idx;
    uint64_t leaf_flags = (flags | PTE_PRESENT) & ~PTE_HUGE;

    if (page_size == PAGE_SIZE_1G)
    {
        leaf_table = pdpt;
        leaf_idx   = VMM_PDPT_INDEX(vaddr);
        leaf_flags |= PTE_HUGE;
    }
    else
    {
        pte_t *pd = next_level(pdpt, VMM_PDPT_INDEX(vaddr), true, &err);
        if (!pd)
            return err;

        if (page_size == PAGE_SIZE_2M)
        {
            leaf_table = pd;
            leaf_idx   = VMM_PD_INDEX(vaddr);
            leaf_flags |= PTE_HUGE;
        }
        else
        {
            pte_t *pt = next_level(pd, VMM_PD_INDEX(vaddr), true, &err);
            if (!pt)
                return err;
            leaf_table = pt;
            leaf_idx   = VMM_PT_INDEX(vaddr);
        }
    }

    pte_t old = leaf_table[leaf_idx];
    pte_t new = paddr | leaf_flags;

    if (old & PTE_PRESENT)
    {
        if (old == new)
            return VMM_OK;

        // A large leaf must not silently discard a lower-level table
        if (page_size != PAGE_SIZE && !(old & PTE_HUGE))
            return VMM_ERR_ALREADY;

        kprintf("[VMM] WARNING: Remapping 0x%lx (was phys 0x%lx)\n",
                vaddr, old & PTE_ADDR_MASK);
    }

    leaf_table[leaf_idx] = new;
    vmm_flush_tlb(vaddr);

    if (page_size == PAGE_SIZE_1G)       map_count_1g++;
    else if (page_size == PAGE_SIZE_2M)  map_count_2m++;
    else                                 map_count_4k++;

    return VMM_OK;
}

/**
 * @brief Locate the leaf entry for @p vaddr (any page size).
 *
 * @param[out] page_size  Size of the page the entry maps.
 * @return Pointer to the leaf entry, or @c NULL if not mapped.
 */
static pte_t *find_leaf(uint64_t vaddr, uint64_t *page_size)
{
    pte_t e = kernel_pml4[VMM_PML4_INDEX(vaddr)];
    if (!(e & PTE_PRESENT))
        return NULL;

    pte_t *pdpt = pte_to_virt(e);
    pte_t *ent = &pdpt[VMM_PDPT_INDEX(vaddr)];
    if (!(*ent & PTE_PRESENT))
        return NULL;
    if (*ent & PTE_HUGE)
    {
        *page_size = PAGE_SIZE_1G;
        return ent;
    }

    pte_t *pd = pte_to_virt(*ent);
    ent = &pd[VMM_PD_INDEX(vaddr)];
    if (!(*ent & PTE_PRESENT))
        return NULL;
    if (*ent & PTE_HUGE)
    {
        *page_size = PAGE_SIZE_2M;
        return ent;
    }

    pte_t *pt = pte_to_virt(*ent);
    ent = &pt[VMM_PT_INDEX(vaddr)];
    if (!(*ent & PTE_PRESENT))
        return NULL;

    *page_size = PAGE_SIZE;
    return ent;
}

/**
 * Unmap the page that starts at @p vaddr: a 4 KiB page, or a whole
 * 2 MiB / 1 GiB page if @p vaddr is its first byte. Caller holds
 * @c vmm_lock
 */
static uint64_t unmap_locked(uint64_t vaddr)
{
    uint64_t size;
    pte_t *ent = find_leaf(vaddr, &size);
    if (!ent)
        return 0;

    if (vaddr & (size - 1))
    {
        kprintf("[VMM] WARNING: cannot unmap part of a large page at 0x%lx\n",
                vaddr);
        return 0;
    }

    uint64_t paddr = *ent & PTE_ADDR_MASK;
    *ent = 0;
    vmm_flush_tlb(vaddr);
    return paddr;
}

/// @c vmm_map_range body. Caller holds @c vmm_lock.
static int map_range_locked(uint64_t vaddr, uint64_t paddr, size_t size,
                            uint64_t flags)
{
    uint64_t end = vaddr + size;

    while (vaddr < end)
    {
        uint64_t left = end - vaddr;
        uint64_t step = PAGE_SIZE;

        if (has_1g_pages && left >= PAGE_SIZE_1G
            && ((vaddr | paddr) & (PAGE_SIZE_1G - 1)) == 0)
            step = PAGE_SIZE_1G;
        else if (left >= PAGE_SIZE_2M
                 && ((vaddr | paddr) & (PAGE_SIZE_2M - 1)) == 0)
            step = PAGE_SIZE_2M;
    
        int ret = map_one_locked(vaddr, paddr, flags, step);
        if (ret != VMM_OK)
            return ret;

        vaddr += step;
        paddr += step;
    }
    return VMM_OK;
}

// ── Initialization ─────────────────────────────────────────────────────

/// CPUID.80000001h:EDX[26] → 1 GiB pages.
static bool cpu_has_1g_pages(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000));
    if (eax < 0x80000001)
        return false;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001));
    return (edx & (1U << 26)) != 0;
}

/// Map a page-aligned slice of the kernel image (boot sections: physical).
static void map_boot_section(const char *start, const char *end,
                             uint64_t flags)
{
    uint64_t pstart = PAGE_ALIGN_DOWN((uintptr_t)start);
    uint64_t pend   = PAGE_ALIGN_UP((uintptr_t)end);
    map_range_locked(KERNEL_VIRTUAL_BASE + pstart, pstart,
                     pend - pstart, flags);
}

/// Map a page-aligned slice of the kernel image (higher-half: virtual).
static void map_image_section(const char *start, const char *end,
                              uint64_t flags)
{
    uint64_t vstart = PAGE_ALIGN_DOWN((uintptr_t)start);
    uint64_t vend   = PAGE_ALIGN_UP((uintptr_t)end);
    map_range_locked(vstart, vstart - KERNEL_VIRTUAL_BASE,
                     vend - vstart, flags);
}

void vmm_init(void)
{
    has_1g_pages = cpu_has_1g_pages();

    // The new tables are built while the boot tables are live; every
    // table frame must therefore be reachable through the boot window
    uint64_t pml4_phys = pmm_alloc_page_zero();
    if (pml4_phys == PMM_ALLOC_FAILED)
    {
        panic("[VMM] cannot allocate PML4");
    }
    kernel_pml4 = (pte_t *)pmm_phys_to_virt(pml4_phys);

    uint64_t flags;
    spin_lock_irqsave(&vmm_lock, &flags);

    // ── Kernel image per-section permissions ────────────
    //
    // .boot_page_tables sits directly below the boot stack
    // and becomes its guard page
    map_boot_section(__boot_start, __boot_end, PTE_KERNEL_ROX);
    map_boot_section(__boot_stack_start, __boot_stack_end, PTE_KERNEL_RW);
    map_boot_section(_ap_trampoline_lma, __ap_trampoline_end, PTE_KERNEL_RO);

    map_image_section(__text_start, __text_end, PTE_KERNEL_ROX);
    map_image_section(__rodata_start, __rodata_end, PTE_KERNEL_RO);
    map_image_section(__data_start, _kernel_virtual_end, PTE_KERNEL_RW);

    // ── Identity map for AP startup ─────────────────────
    //
    // The AP trampoline (0x8000), the boot GDT, and
    // .ap_trampoline all reside below 2 MiB and are
    // executed/read at their physical addresses while an
    // AP enables paging. Page 0 stays unmapped.
    //
    // TODO: remove after SMP bring-up (APs only need it
    //       transiently)
    map_range_locked(PAGE_SIZE, PAGE_SIZE, PAGE_SIZE_2M - PAGE_SIZE,
                     PTE_KERNEL_RWX);

    // ── Direct map of all RAM ───────────────────────────
    size_t n_4k = map_count_4k, n_2m = map_count_2m, n_1g = map_count_1g;
    uint64_t mapped_bytes = 0;

    for (size_t i = 0; i < pmm_region_count(); ++i)
    {
        const struct pmm_region *r = pmm_get_region(i);
        uint64_t start = PAGE_ALIGN_DOWN(r->start);
        uint64_t end   = PAGE_ALIGN_UP(r->end);
    
        int ret = map_range_locked(DIRECT_MAP_BASE + start, start,
                                   end - start, PTE_KERNEL_RW);
        if (ret != VMM_OK)
            kprintf("[VMM] ERROR: direct map of 0x%lx-0x%lx failed (%d)\n",
                    start, end, ret);
        mapped_bytes += end - start;
    }

    spin_unlock_irqrestore(&vmm_lock, flags);

    // ── Switch ──────────────────────────────────────────
    //
    // RIP (.text), RSP (boot stack), and the GDT (identity)
    // are all mapped at the same virtual addresses in the
    // new tables. Nothing may touch a boot-window address
    // other than the kernel image after this.
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

    pmm_enable_direct_map();
    kernel_pml4 = (pte_t *)PHYS_TO_VIRT(pml4_phys);

    kprintf("[VMM] Kernel page tables active, PML4 at phys 0x%lx\n",
            pml4_phys);
    kprintf("[VMM] Direct map: %lu MiB at 0x%lx "
            "(%zu * 1G, %zu * 2M, %zu * 4K pages)%s\n",
            mapped_bytes >> 20, DIRECT_MAP_BASE,
            map_count_1g - n_1g, map_count_2m - n_2m, map_count_4k - n_4k,
            has_1g_pages ? "" : ", no 1G page support");
}

bool vmm_has_1g_pages(void)
{
    return has_1g_pages;
}

// ── Mapping ────────────────────────────────────────────────────────────

int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);
    int ret = map_one_locked(PAGE_ALIGN_DOWN(vaddr), PAGE_ALIGN_DOWN(paddr),
                             flags, PAGE_SIZE);
    spin_unlock_irqrestore(&vmm_lock, irq);
    return ret;
}

int vmm_map_pages(uint64_t vaddr, uint64_t paddr, size_t count, uint64_t flags)
{
    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);

    for (size_t i = 0; i < count; ++i)
    {
        int ret = map_one_locked(vaddr + i * PAGE_SIZE,
                                 paddr + i * PAGE_SIZE,
                                 flags, PAGE_SIZE);
        if (ret != VMM_OK)
        {
            // Roll back entries mapped before the failure
            for (size_t j = 0; j < i; ++j)
                unmap_locked(vaddr + j * PAGE_SIZE);
            spin_unlock_irqrestore(&vmm_lock, irq);
            return ret;
        }
    }

    spin_unlock_irqrestore(&vmm_lock, irq);
    return VMM_OK;
}

int vmm_map_range(uint64_t vaddr, uint64_t paddr, size_t size, uint64_t flags)
{
    if (!PAGE_ALIGNED(vaddr) || !PAGE_ALIGNED(paddr) || !PAGE_ALIGNED(size))
        return VMM_ERR_INVALID;

    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);
    int ret = map_range_locked(vaddr, paddr, size, flags);
    spin_unlock_irqrestore(&vmm_lock, irq);
    return ret;
}

void *vmm_map_mmio(uint64_t paddr, size_t size)
{
    if (size == 0)
        return NULL;

    uint64_t base  = PAGE_ALIGN_DOWN(paddr);
    uint64_t pages = (PAGE_ALIGN_UP(paddr + size) - base) / PAGE_SIZE;

    // Reserve virtual space (plus one unmapped guard page)
    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);
    uint64_t vaddr = mmio_next;
    bool fits = vaddr + (pages + 1) * PAGE_SIZE
                <= MMIO_VIRTUAL_BASE + MMIO_VIRTUAL_SIZE;
    if (fits)
        mmio_next += (pages + 1) * PAGE_SIZE;
    spin_unlock_irqrestore(&vmm_lock, irq);

    if (!fits)
    {
        kprintf("[VMM] ERROR: MMIO window exhausted\n");
        return NULL;
    }

    if (vmm_map_pages(vaddr, base, pages, PTE_MMIO) != VMM_OK)
        return NULL;

    return (void *)(vaddr + (paddr - base));
}

// ── Unmapping ──────────────────────────────────────────────────────────

uint64_t vmm_unmap_page(uint64_t vaddr)
{
    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);
    uint64_t paddr = unmap_locked(PAGE_ALIGN_DOWN(vaddr));
    spin_unlock_irqrestore(&vmm_lock, irq);
    return paddr;
}

void vmm_unmap_pages(uint64_t vaddr, size_t count)
{
    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);
    for (size_t i = 0; i < count; ++i)
        unmap_locked(vaddr + i * PAGE_SIZE);
    spin_unlock_irqrestore(&vmm_lock, irq);
}

// ── Queries ────────────────────────────────────────────────────────────

uint64_t vmm_get_physical(uint64_t vaddr)
{
    uint64_t size;
    pte_t *ent = find_leaf(vaddr, &size);
    if (!ent)
        return 0;
    return (*ent & PTE_ADDR_MASK) + (vaddr & (size - 1));
}

uint64_t virt_to_phys(const void *ptr)
{
    uint64_t vaddr = (uint64_t)ptr;

    if (vaddr >= DIRECT_MAP_BASE && vaddr < DIRECT_MAP_BASE + DIRECT_MAP_SIZE)
        return vaddr - DIRECT_MAP_BASE;
    if (vaddr >= KERNEL_VIRTUAL_BASE && vaddr < (uint64_t)_kernel_virtual_end)
        return vaddr - KERNEL_VIRTUAL_BASE;

    return vmm_get_physical(vaddr);
}

bool vmm_is_mapped(uint64_t vaddr)
{
    uint64_t size;
    return find_leaf(vaddr, &size) != NULL;
}

uint64_t vmm_get_flags(uint64_t vaddr)
{
    uint64_t size;
    pte_t *ent = find_leaf(vaddr, &size);
    return ent ? (*ent & ~PTE_ADDR_MASK) : 0;
}

// ── Flag modification ──────────────────────────────────────────────────

int vmm_set_flags(uint64_t vaddr, uint64_t flags)
{
    vaddr = PAGE_ALIGN_DOWN(vaddr);

    uint64_t irq;
    spin_lock_irqsave(&vmm_lock, &irq);

    uint64_t size;
    pte_t *ent = find_leaf(vaddr, &size);
    if (!ent)
    {
        spin_unlock_irqrestore(&vmm_lock, irq);
        return VMM_ERR_NOT_MAPPED;
    }

    uint64_t keep = PTE_PRESENT | (size != PAGE_SIZE ? PTE_HUGE: 0);
    *ent = (*ent & PTE_ADDR_MASK) | (flags & ~PTE_HUGE) | keep;
    vmm_flush_tlb(vaddr);

    spin_unlock_irqrestore(&vmm_lock, irq);
    return VMM_OK;
}

