/**
 * @file test_main.c
 * @brief Test runner entry point.
 *
 * Called by @c kernel_main() when @c KERNEL_TEST is defined. Initializes
 * subsystems in dependency order, runs every registered test suite,
 * prints a summary, and terminates QEMU with an appropriate exit code.
 */

#include "test.h"
#include "multiboot2.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "slab.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "timer.h"
#include "pci.h"
#include "virtio_net.h"
#include "netbuf.h"
#include "netdev.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"

// v0.0.1 suites
extern uint32_t test_types(void);
extern uint32_t test_string(void);

// v0.0.2 suites
extern uint32_t test_pmm(void);
extern uint32_t test_vmm(void);
extern uint32_t test_heap(void);
extern uint32_t test_slab(void);

// v0.0.3 suite
extern uint32_t test_interrupts(void);

// v0.0.4 suite
extern uint32_t test_pci_net(void);

// v0.0.5 suites
extern uint32_t test_netbuf(void);
extern uint32_t test_net(void);

/**
 * @brief Run all test suites and exit QEMU.
 *
 * Does not return — terminates QEMU via the isa-debug-exit device.
 *
 * @param mb_info  Multiboot2 info pointer.
 */
NORETURN void test_main(struct multiboot2_info *mb_info)
{
    kprintf("\n==== eMMK test runner ====\n\n");

    uint32_t failures = 0;

    // ── v0.0.1 (no MM dependencies) ─────────────────────
    failures += test_types();
    failures += test_string();

    // ── Initialize memory subsystem for v0.0.2+ ─────────
    struct multiboot2_tag_mmap *mmap =
        (struct multiboot2_tag_mmap *)
        multiboot2_get_tag(mb_info, MULTIBOOT2_TAG_TYPE_MMAP);

    if (!mmap)
    {
        kprintf("ERROR: No memory map - cannot continue\n");
        qemu_exit_failure();
    }

    uint64_t mb_info_phys = VIRT_TO_PHYS_ADDR((uintptr_t)mb_info);
    uint64_t mb_info_end  = mb_info_phys + mb_info->total_size;
    pmm_init(mmap, mb_info_end);
    vmm_init();
    heap_init();
    slab_init();
    kprintf("\n");

    // ── v0.0.2 ──────────────────────────────────────────
    failures += test_pmm();
    failures += test_vmm();
    failures += test_heap();
    failures += test_slab();

    // ── Initialize interrupt subsystems ─────────────────
    idt_init();
    pic_init();
    pic_disable();
    lapic_init();
    ioapic_init(IOAPIC_BASE_PHYS_DEFAULT);
    timer_init();
    kprintf("\n");

    // ── v0.0.3 ──────────────────────────────────────────
    failures += test_interrupts();

    // ── Initialize PCI ──────────────────────────────────
    pci_init();
    kprintf("\n");

    // ── v0.0.4 ──────────────────────────────────────────
    failures += test_pci_net();

    // ── Initialize network stack ────────────────────────
    netbuf_init();
    netdev_init();
    arp_init();
    ip_init();
    icmp_init();
    kprintf("\n");

    // ── v0.0.5 ──────────────────────────────────────────
    failures += test_netbuf();
    failures += test_net();

    // ── Summary ─────────────────────────────────────────
    kprintf("==========================\n");

    if (failures == 0)
    {
        kprintf("all tests passed\n");
        qemu_exit_success();
    }
    else
    {
        kprintf("%u total failure(s)\n", failures);
        qemu_exit_failure();
    }
}

