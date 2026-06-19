/**
 * @file vmm.c
 * @brief Virtual Memory Manager - 4-level page table operations.
 */

#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

/// Virtual pointer to the kernel's top-level page table (PML4).
static pte_t *kernel_pml4;

// ── Internal helpers ───────────────────────────────────────────────────

/// Extract the physical address from a PTE and convert to a virtual pointer.
static inline pte_t *pte_to_virt(pte_t entry)
{
    return (pte_t *)PHYS_TO_VIRT(entry & PTE_ADDR_MASK);
}

/**
 * @brief Allocate a zeroed 4 KiB frame for a new page table.
 * @return Virtual pointer to the new table, or @c NULL on OOM.
 */
static pte_t *alloc_page_table(void)
{
    uint64_t paddr = pmm_alloc_page_zero();
    if (paddr == PMM_ALLOC_FAILED)
    {
        kprintf("[VMM] ERROR: Failed to allocate page table frame\n");
        return NULL;
    }
    return (pte_t *)PHYS_TO_VIRT(paddr);
}

/**
 * @brief Walk (or create) one level of the page table hierarchy.
 *
 * @param table   Current-level table (virtual pointer).
 * @param index   Index within @p table.
 * @param create  If @c true, allocate a new table when the entry is 
 *                not present.
 *
 * @return Virtual pointer to the next-level table, or @c NULL if:
 *         - the entry is not present and @p create is @c false,
 *         - the entry is a huge page (cannot descend further), or
 *         - allocation failed.
 */
static pte_t *vmm_get_next_level(pte_t *table, size_t index, bool create)
{
    pte_t *entry = &table[index]; 

    if (!(*entry & PTE_PRESENT))
    {
        if (!create)
            return NULL;

        pte_t *new_table = alloc_page_table();
        if (!new_table)
            return NULL;

        *entry = VIRT_TO_PHYS_ADDR((uintptr_t)new_table)
                 | PTE_PRESENT | PTE_WRITABLE;
    }

    // Cannot descend through a huge page entry
    if (*entry & PTE_HUGE)
        return NULL;

    return pte_to_virt(*entry);
}

/**
 * @brief Walk the page table hierarchy down to the PT level.
 *
 * Does @b not create intermediate levels — read-only walk.
 *
 * @param vaddr         Virtual address to resolve.
 * @param[out] out_pt   Receives the virtual pointer to the final PT.
 * @param[out] out_idx  Receives the PT index within that table.
 *
 * @return @c VMM_OK if a 4 KiB PTE was reached, or a @c VMM_ERR_* code.
 *         Returns @c VMM_ERR_INVALID if a huge page was encountered
 *         (the caller must handle huge pages separately).
 */
static int vmm_walk_to_pt(uint64_t vaddr, pte_t **out_pt, size_t *out_idx)
{
    size_t pml4_idx = VMM_PML4_INDEX(vaddr);
    size_t pdpt_idx = VMM_PDPT_INDEX(vaddr);
    size_t pd_idx   = VMM_PD_INDEX(vaddr);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;
    pte_t *pdpt = pte_to_virt(kernel_pml4[pml4_idx]);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;
    if (pdpt[pdpt_idx] & PTE_HUGE)
        return VMM_ERR_INVALID;
    pte_t *pd = pte_to_virt(pdpt[pdpt_idx]);

    if (!(pd[pd_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;
    if (pd[pd_idx] & PTE_HUGE)
        return VMM_ERR_INVALID;
    pte_t *pt = pte_to_virt(pd[pd_idx]);

    *out_pt  = pt;
    *out_idx = VMM_PT_INDEX(vaddr);
    return VMM_OK;
}

// ── Initialization ─────────────────────────────────────────────────────

void vmm_init(void)
{
    uint64_t pml4_phys = vmm_get_pml4();
    kernel_pml4 = (pte_t *)PHYS_TO_VIRT(pml4_phys);

    kprintf("[VMM] Initialized with PML4 at phys 0x%lx, virt %p\n",
            pml4_phys, (void *)kernel_pml4);
    kprintf("[VMM] Higher-half base: 0x%lx\n", KERNEL_VIRTUAL_BASE);
}

// ── Mapping ────────────────────────────────────────────────────────────

int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    vaddr = PAGE_ALIGN_DOWN(vaddr);
    paddr = PAGE_ALIGN_DOWN(paddr);

    // Walk / create the four levels
    pte_t *pdpt = vmm_get_next_level(kernel_pml4, VMM_PML4_INDEX(vaddr), true);
    if (!pdpt) return VMM_ERR_NO_MEMORY;

    pte_t *pd = vmm_get_next_level(pdpt, VMM_PDPT_INDEX(vaddr), true);
    if (!pd) return VMM_ERR_NO_MEMORY;

    pte_t *pt = vmm_get_next_level(pd, VMM_PD_INDEX(vaddr), true);
    if (!pt) return VMM_ERR_NO_MEMORY;

    size_t pt_idx = VMM_PT_INDEX(vaddr);

    if (pt[pt_idx] & PTE_PRESENT)
        kprintf("[VMM] WARNING: Remapping 0x%lx (was phys 0x%lx)\n",
                vaddr, pt[pt_idx] & PTE_ADDR_MASK);

    pt[pt_idx] = paddr | flags | PTE_PRESENT;
    vmm_flush_tlb(vaddr);

    return VMM_OK;
}

int vmm_map_pages(uint64_t vaddr, uint64_t paddr, size_t count, uint64_t flags)
{
    for (size_t i = 0; i < count; ++i)
    {
        int ret = vmm_map_page(vaddr + i * PAGE_SIZE,
                               paddr + i * PAGE_SIZE,
                               flags);
        if (ret != VMM_OK)
        {
            // Roll back entries mapped before the failure
            for (size_t j = 0; j < i; ++j)
                vmm_unmap_page(vaddr + j * PAGE_SIZE);
            return ret;
        }
    }
    return VMM_OK;
}

// ── Unmapping ──────────────────────────────────────────────────────────

uint64_t vmm_unmap_page(uint64_t vaddr)
{
    vaddr = PAGE_ALIGN_DOWN(vaddr);

    pte_t *pt;
    size_t pt_idx;
    int ret = vmm_walk_to_pt(vaddr, &pt, &pt_idx);

    if (ret == VMM_ERR_INVALID)
    {
        kprintf("[VMM] WARNING: Cannot unmap within a huge page at 0x%lx\n",
                vaddr);
        return 0;
    }
    if (ret != VMM_OK)
        return 0;

    if (!(pt[pt_idx] & PTE_PRESENT))
        return 0;

    uint64_t paddr = pt[pt_idx] & PTE_ADDR_MASK;
    pt[pt_idx] = 0;
    vmm_flush_tlb(vaddr);

    return paddr;
}

void vmm_unmap_pages(uint64_t vaddr, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        vmm_unmap_page(vaddr + i * PAGE_SIZE);
}

// ── Queries ────────────────────────────────────────────────────────────

uint64_t vmm_get_physical(uint64_t vaddr)
{
    size_t pml4_idx = VMM_PML4_INDEX(vaddr);
    size_t pdpt_idx = VMM_PDPT_INDEX(vaddr);
    size_t pd_idx   = VMM_PD_INDEX(vaddr);
    size_t pt_idx   = VMM_PT_INDEX(vaddr);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
        return 0;
    pte_t *pdpt = pte_to_virt(kernel_pml4[pml4_idx]);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT))
        return 0;
    if (pdpt[pdpt_idx] & PTE_HUGE)  // 1 GiB page
        return (pdpt[pdpt_idx] & PTE_ADDR_MASK) + (vaddr & 0x3FFFFFFF);
    pte_t *pd = pte_to_virt(pdpt[pdpt_idx]);

    if (!(pd[pd_idx] & PTE_PRESENT))
        return 0;
    if (pd[pd_idx] & PTE_HUGE)      // 2 MiB page
        return (pd[pd_idx] & PTE_ADDR_MASK) + (vaddr & 0x1FFFFF);
    pte_t *pt = pte_to_virt(pd[pd_idx]);

    if (!(pt[pt_idx] & PTE_PRESENT))
        return 0;

    return (pt[pt_idx] & PTE_ADDR_MASK) + VMM_PAGE_OFFSET(vaddr);
}

bool vmm_is_mapped(uint64_t vaddr)
{
    size_t pml4_idx = VMM_PML4_INDEX(vaddr);
    size_t pdpt_idx = VMM_PDPT_INDEX(vaddr);
    size_t pd_idx   = VMM_PD_INDEX(vaddr);
    size_t pt_idx   = VMM_PT_INDEX(vaddr);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
        return false;
    pte_t *pdpt = pte_to_virt(kernel_pml4[pml4_idx]);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT))
        return false;
    if (pdpt[pdpt_idx] & PTE_HUGE)
        return true;
    pte_t *pd = pte_to_virt(pdpt[pdpt_idx]);

    if (!(pd[pd_idx] & PTE_PRESENT))
        return false;
    if (pd[pd_idx] & PTE_HUGE)
        return true;
    pte_t *pt = pte_to_virt(pd[pd_idx]);

    return (pt[pt_idx] & PTE_PRESENT) != 0;
}

// ── Flag modification ──────────────────────────────────────────────────

int vmm_set_flags(uint64_t vaddr, uint64_t flags)
{
    vaddr = PAGE_ALIGN_DOWN(vaddr);

    size_t pml4_idx = VMM_PML4_INDEX(vaddr);
    size_t pdpt_idx = VMM_PDPT_INDEX(vaddr);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;
    pte_t *pdpt = pte_to_virt(kernel_pml4[pml4_idx]);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    if (pdpt[pdpt_idx] & PTE_HUGE)  // 1 GiB page
    {
        pdpt[pdpt_idx] = (pdpt[pdpt_idx] & PTE_ADDR_MASK)
                         | flags | PTE_PRESENT | PTE_HUGE;
        vmm_flush_tlb(vaddr);
        return VMM_OK;
    }

    pte_t *pd = pte_to_virt(pdpt[pdpt_idx]);
    size_t pd_idx = VMM_PD_INDEX(vaddr);

    if (!(pd[pd_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    if (pd[pd_idx] & PTE_HUGE)  // 2 MiB page
    {
        pd[pd_idx] = (pd[pd_idx] & PTE_ADDR_MASK)
                     | flags | PTE_PRESENT | PTE_HUGE;
        vmm_flush_tlb(vaddr);
        return VMM_OK;
    }

    pte_t *pt = pte_to_virt(pd[pd_idx]);
    size_t pt_idx = VMM_PT_INDEX(vaddr);

    if (!(pt[pt_idx] & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    // Standard 4 KiB page
    pt[pt_idx] = (pt[pt_idx] & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    vmm_flush_tlb(vaddr);

    return VMM_OK;
}

