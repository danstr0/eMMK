/**
 * @file main.c
 * @brief Kernel entry point (C side).
 *
 * Initializes all subsystems in dependency order, brings up all CPU
 * cores, probes PCI drivers (every virtio-net NIC becomes a netdev),
 * configures IPv4 on each NIC, and enters the main run-to-completion
 * poll loop.
 *
 * @par Build modes
 * - Default: single-core poll loop
 * - @c KERNEL_TEST: test runner (see test/test_main.c)
 * - @c KERNEL_DEMO: multi-core distributed pipeline with UDP echo
 *   server and per-core benchmarking.
 */

#include "types.h"
#include "serial.h"
#include "panic.h"
#include "multiboot2.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "slab.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "timer.h"
#include "tsc.h"
#include "tss.h"
#include "percpu.h"
#include "smp.h"
#include "pci.h"
#include "virtio_net.h"
#include "netbuf.h"
#include "netdev.h"
#include "ethernet.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "string.h"

#ifdef KERNEL_DEMO
#include "dispatch.h"
#include "bench.h"
#include "log.h"
#endif

#ifdef KERNEL_TEST
extern NORETURN void test_main(struct multiboot2_info *mb_info);
#endif

// Linker-provided symbols (see linker.ld)
extern char _kernel_physical_start[];
extern char _kernel_physical_end[];
extern char _kernel_virtual_start[];
extern char _kernel_virtual_end[];

// ── Boot output helpers ────────────────────────────────────────────────

/// Human-readable names for Multiboot2 memory-map region types.
static const char *mmap_type_name(uint32_t type)
{
    switch (type)
    {
        case MULTIBOOT2_MEMORY_AVAILABLE:        return "available";
        case MULTIBOOT2_MEMORY_RESERVED:         return "reserved";
        case MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE: return "ACPI reclaimable";
        case MULTIBOOT2_MEMORY_NVS:              return "ACPI NVS";
        case MULTIBOOT2_MEMORY_BADRAM:           return "bad RAM";
        default:                                 return "unknown";
    }
}

/// Print every region in the Multiboot2 memory map.
static void print_memory_map(struct multiboot2_tag_mmap *mmap)
{
    kprintf("Memory map (%u-byte entries):\n", mmap->entry_size);

    struct multiboot2_mmap_entry *entry;
    multiboot2_mmap_foreach(mmap, entry)
    {
        kprintf("  0x%lx  %lu KiB  %s\n",
                entry->addr,
                entry->len / 1024,
                mmap_type_name(entry->type));
    }
}

// ── Network configuration ──────────────────────────────────────────────

/// Give every virtio-net device its IPv4 address.
static void net_configure(void)
{
    for (int i = 0; i < virtio_net_count(); ++i)
    {
        struct netdev *dev = virtio_net_get(i)->netdev;
        netdev_set_ip(dev,
                      IP4_ADDR(10, 0, 2 + i, 15),
                      IP4_ADDR(255, 255, 255, 0),
                      IP4_ADDR(10, 0, 2 + i, 2));
    }
}

// ── Demo: UDP echo handler ─────────────────────────────────────────────

#ifdef KERNEL_DEMO

#define ECHO_PORT  7

/// UDP echo handler — reflects datagrams back to the sender.
static void udp_echo_handler(struct netbuf *nb)
{
    // Echo in place: this buffer, rewritten, is the reply
    if (nb->len > 0 && nb->len <= 1400)
        udp_reply(nb);
    else
        netbuf_free(nb);
}

///  Wraps @c dispatch_worker_loop with per-packet TSC measurement.
static NORETURN void bench_worker_loop(int core_index)
{
    struct spsc_ring *ring = dispatch_get_core_ring(core_index);

    if (!ring)
    {
        kprintf("[DEMO] Worker core %d has no ring\n", core_index);
        for (;;) __asm__ volatile("sti; hlt");
    }

    kprintf("[DEMO] Worker running on core %d\n", core_index);

    for (;;)
    {
        void *ptr;
        while (spsc_ring_dequeue(ring, &ptr))
        {
            struct netbuf *nb = (struct netbuf *)ptr;
            uint64_t t0 = tsc_read();
            eth_input(nb);
            uint64_t t1 = tsc_read();
            bench_record(core_index, t1 - t0);
        }
        __asm__ volatile("pause");
    }
}

/// Report the buffer counters: with zero-copy echo, packets flow through
/// the pools while slow_allocs stays flat.
static void demo_report_buffers(void)
{
    uint64_t pool_allocs = 0, remote_frees = 0, fails = 0;

    for (int i = 0; i < virtio_net_count(); ++i)
    {
        struct netbuf_pool *pool = virtio_net_get(i)->rx_pool;
        pool_allocs  += pool->allocs;
        remote_frees += pool->remote_frees;
        fails        += pool->alloc_fails;
    }

    kprintf("[NETBUF] slow_allocs=%lu pool_allocs=%lu remote_frees=%lu "
            "pool_empty=%lu\n", netbuf_slow_allocs(), pool_allocs,
            remote_frees, fails);
}

/// Distributor loop with periodic benchmark reporting.
static NORETURN void bench_distributor_loop(int core_index)
{
    (void)core_index;

    kprintf("[DEMO] Distributor running on core %d\n", core_index);

    uint64_t next_report = tsc_read() + tsc_get_freq() * 5;

    for (;;)
    {
        dispatch_rx();

        // Report every ~5 seconds
        uint64_t now = tsc_read();
        if (now >= next_report)
        {
            bench_report();
            demo_report_buffers();
            next_report = now + tsc_get_freq() * 5;
        }

        __asm__ volatile("pause");
    }
}

#endif // KERNEL_DEMO

// ── Entry point ────────────────────────────────────────────────────────

/**
 * @brief C entry point.
 *
 * @param mb_info  Higher-half pointer to the Multiboot2 info structure
 *                 (converted from the physical address by entry64.asm).
 */
NORETURN void kernel_main(struct multiboot2_info *mb_info)
{
    percpu_init(0, 0);

    serial_init(COM1_PORT, 115200);

#ifdef KERNEL_TEST
    test_main(mb_info);
    // test_main does not return
#endif

    kprintf("\neMMK booting...\n\n");

    // ── Bootloader / command line ───────────────────────
    struct multiboot2_tag_bootloader *bl =
        (struct multiboot2_tag_bootloader *)
        multiboot2_get_tag(mb_info, MULTIBOOT2_TAG_TYPE_BOOT_LOADER_NAME);
    if (bl)
        kprintf("Bootloader: %s\n", bl->string);

    struct multiboot2_tag_cmdline *cmd =
        (struct multiboot2_tag_cmdline *)
        multiboot2_get_tag(mb_info, MULTIBOOT2_TAG_TYPE_CMDLINE);
    if (cmd)
        kprintf("cmdline:    %s\n", cmd->string);

    // ── Kernel layout ───────────────────────────────────
    kprintf("\nKernel physical: 0x%lx - 0x%lx (%lu KiB)\n",
            (uintptr_t)_kernel_physical_start,
            (uintptr_t)_kernel_physical_end,
            ((uintptr_t)_kernel_physical_end
             - (uintptr_t)_kernel_physical_start) / 1024);

    kprintf("Kernel virtual:  0x%lx - 0x%lx\n",
            (uintptr_t)_kernel_virtual_start,
            (uintptr_t)_kernel_virtual_end);

    // ── Memory map ──────────────────────────────────────
    struct multiboot2_tag_mmap *mmap =
        (struct multiboot2_tag_mmap *)
        multiboot2_get_tag(mb_info, MULTIBOOT2_TAG_TYPE_MMAP);
    if (mmap)
    {
        kprintf("\n");
        print_memory_map(mmap);
    }

    // ── Memory subsystems ───────────────────────────────
    kprintf("\n");
    if (!mmap)
    {
        panic("no Multiboot2 memory map");
    }

    uint64_t mb_info_phys = VIRT_TO_PHYS_ADDR((uintptr_t)mb_info);
    uint64_t mb_info_end  = mb_info_phys + mb_info->total_size;
    pmm_init(mmap, mb_info_phys, mb_info_end);
    vmm_init();

    // The boot window that mb_info pointed to is gone; re-point it at
    // the direct map (still needed for ACPI discovery)
    mb_info = (struct multiboot2_info *)PHYS_TO_VIRT(mb_info_phys);
    mmap = NULL;
    heap_init();
    slab_init();

    // ── Interrupt subsystems ──────────────────────────── 
    kprintf("\n");
    idt_init();
    pic_init();
    lapic_init();
    // TODO: IOAPIC address will eventually come from ACPI MADT
    ioapic_init(IOAPIC_BASE_PHYS_DEFAULT);
    tsc_init();
    timer_init();

    // ── TSS, per-CPU, and SMP ───────────────────────────
    kprintf("\n");
    tss_init();
    idt_configure_ist();
    kprintf("\n");
    smp_init();

    // ── Network stack ───────────────────────────────────
    kprintf("\n");
    netbuf_init();
    netdev_init();
    arp_init();
    ip_init();
    icmp_init();
    udp_init();

    // ── PCI and drivers ─────────────────────────────────
    kprintf("\n");
    virtio_net_register();
    pci_init();
    kprintf("\n");
    net_configure();

    // ── Summary ─────────────────────────────────────────
    struct pmm_stats ps;
    pmm_get_stats(&ps);
    kprintf("\n[SUMMARY] PMM: %lu / %lu pages free (%lu MiB / %lu MiB)\n",
            ps.free_pages, ps.total_pages,
            ps.free_memory / (1024 * 1024),
            ps.total_memory / (1024 * 1024));

    struct heap_stats hs;
    heap_get_stats(&hs);
    kprintf("[SUMMARY] Heap: %lu KiB total, %lu KiB used, %lu KiB free\n",
            (uint64_t)(hs.total_size / 1024),
            (uint64_t)(hs.used_size / 1024),
            (uint64_t)(hs.free_size / 1024));

    kprintf("[SUMMARY] Clock: TSC %lu MHz, tickless, uptime %lu ms\n",
            tsc_get_freq() / 1000000, timer_get_ms());

    for (int i = 0; i < virtio_net_count(); ++i)
    {
        struct virtio_net_dev *vnet = virtio_net_get(i);
        kprintf("[SUMMARY] Network: %s  MAC=%s IP=%s vector=%u\n",
                vnet->netdev->name, ETH_FMT(vnet->mac),
                IP4_FMT(vnet->netdev->ip_addr), vnet->irq_vector);
    }
    if (virtio_net_count() == 0)
        kprintf("[SUMMARY] Network: no device\n");

    kprintf("\nBoot complete.\n");

    // ════════════════════════════════════════════════════
    // DEMO: multi-core distributed pipeline with UDP echo
    // ════════════════════════════════════════════════════
#ifdef KERNEL_DEMO

    int cores = smp_get_core_count();
    int num_workers = cores - 1;

    if (num_workers < 1)
    {
        kprintf("[DEMO] Need at least 2 cores (-smp 2). "
                "Falling back to default\n");
        goto non_demo;
    }

    // Initialize benchmarking
    bench_init();

    // Configure worker cores
    int worker_ids[DISPATCH_MAX_WORKERS];
    for (int i = 0; i < num_workers; ++i)
        worker_ids[i] = i + 1;

    dispatch_init(worker_ids, num_workers);

    // Bind the UDP echo server
    udp_bind(ECHO_PORT, udp_echo_handler);

    kprintf("\n[DEMO] UDP echo server on port %d\n", ECHO_PORT);
    kprintf("[DEMO] %d worker(s), distributor on core 0\n", num_workers);
    kprintf("[DEMO] Reporting every 5 seconds\n");
    kprintf("[DEMO] Test with: echo hello | nc -u 127.0.0.1 8080\n\n");

    // Assign roles
    smp_assign_role(0, bench_distributor_loop, "distributor");
    for (int i = 0; i < num_workers; ++i)
        smp_assign_role(worker_ids[i], bench_worker_loop, "worker");

    // Wake APs into their worker loops
    smp_launch_all();

    // BSP becomes the distributor
    bench_distributor_loop(0);

non_demo:

#endif // KERNEL_DEMO

    // ══════════════════════════════
    // Default: single-core poll loop
    // ══════════════════════════════

    // ── Test ping to every NIC's gateway ────────────────
    if (virtio_net_count() > 0)
    {
        for (int i = 0; i < virtio_net_count(); ++i)
        {
            struct netdev *dev = virtio_net_get(i)->netdev;
            kprintf("\n[PING] %s: sending echo request to gateway...\n",
                    dev->name);
            icmp_send_echo_request(dev->gateway, 1, (uint16_t)(i + 1),
                                   "eMMK", 4);
        }

        // Poll for the replies
        for (int i = 0; i < 500; ++i)
        {
            netdev_poll_all();
            timer_sleep_ms(1);
        }
    }

    // ── Main poll loop ─────────────────────────────────────────────────
    kprintf("\nEntering poll loop...\n");
    __asm__ volatile("cli");
    for (;;)
    {
        while (netdev_poll_all() > 0)
            ;
        netdev_idle_wait();
    }
}

