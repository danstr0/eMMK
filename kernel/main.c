/**
 * @file main.c
 * @brief Kernel entry point (C side).
 *
 * Initializes all subsystems in dependency order, wires up the
 * virtio-net driver to the network stack, and enters the main
 * run-to-completion poll loop.
 *
 * When compiled with @c -DKERNEL_TEST the normal boot path is replaced
 * by the test runner (see test_main.c).
 */

#include "types.h"
#include "serial.h"
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
#include "string.h"

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

// ── Virtio-net ↔ netdev glue ───────────────────────────────────────────

/**
 * @brief @c netdev_ops.send adapter for virtio-net.
 *
 * Extracts the raw frame from the @c netbuf, passes it to @c virtio_net_send
 * (which copies the data into its own DMA buffer), then frees the
 * @c netbuf. The driver always takes ownership of @p nb.
 */
static int virtio_netdev_send(struct netdev *dev, struct netbuf *nb)
{
    struct virtio_net_dev *vdev = (struct virtio_net_dev *)dev->priv;
    int ret = virtio_net_send(vdev, nb->data, nb->len);
    netbuf_free(nb);
    return ret;
}

/**
 * @brief @c netdev_ops.poll adapter for virtio-net.
 *
 * Calls @c virtio_net_poll, which invokes the RX callback for each
 * completed packet.
 */
static void virtio_netdev_poll(struct netdev *dev)
{
    struct virtio_net_dev *vdev = (struct virtio_net_dev *)dev->priv;
    virtio_net_poll(vdev);
}

static const struct netdev_ops virtio_netdev_ops =
{
    .send = virtio_netdev_send,
    .poll = virtio_netdev_poll,
};

/**
 * @brief virtio-net RX callback — bridges received packets into the
 *        network stack.
 *
 * Called inline by @c virtio_net_poll for each completed RX buffer.
 * Allocates a @c netbuf, copies the packet data, and queues it for
 * stack processing via @c netdev_receive().
 */
static void virtio_rx_callback(const void *data, size_t len, void *ctx)
{
    struct netdev *dev = (struct netdev *)ctx;

    struct netbuf *nb = netbuf_alloc(0);
    if (!nb)
        return;

    void *p = netbuf_put(nb, len);
    if (!p)
    {
        netbuf_free(nb);
        return;
    }
    memcpy(p, data, len);

    netdev_receive(dev, nb);
}

// ── Entry point ────────────────────────────────────────────────────────

/**
 * @brief C entry point.
 *
 * @param mb_info  Higher-half pointer to the Multiboot2 info structure
 *                 (converted from the physical address by entry64.asm).
 */
NORETURN void kernel_main(struct multiboot2_info *mb_info)
{
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
        kprintf("FATAL: No memory map\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    uint64_t mb_info_phys = VIRT_TO_PHYS_ADDR((uintptr_t)mb_info);
    uint64_t mb_info_end  = mb_info_phys + mb_info->total_size;
    pmm_init(mmap, mb_info_end);
    vmm_init();
    heap_init();
    slab_init();

    // ── Interrupt subsystems ──────────────────────────── 
    kprintf("\n");
    idt_init();
    pic_init();
    pic_disable();
    lapic_init();
    ioapic_init(IOAPIC_BASE_PHYS_DEFAULT);
    timer_init();
    tsc_init();

    // ── TSS, per-CPU, and SMP ───────────────────────────
    kprintf("\n");
    tss_init();
    percpu_init_bsp();
    idt_configure_ist();
    kprintf("\n");
    smp_init();

    // ── PCI and virtio-net ──────────────────────────────
    kprintf("\n");
    pci_init();
    kprintf("\n");
    struct virtio_net_dev *vnet = virtio_net_init();

    // ── Network stack ───────────────────────────────────
    kprintf("\n");
    netbuf_init();
    netdev_init();
    arp_init();
    ip_init();
    icmp_init();

    // Wire virtio-net into the netdev abstraction
    struct netdev *eth0 = NULL;
    if (vnet)
    {
        eth0 = netdev_alloc("eth0");
        if (eth0)
        {
            memcpy(eth0->mac, vnet->mac, 6);
            eth0->ops  = &virtio_netdev_ops;
            eth0->priv = vnet;
            netdev_register(eth0);
            netdev_set_up(eth0);

            // QEMU user mode network:
            //   - guest=10.0.2.15
            //   - gw=10.0.2.2
            netdev_set_ip(eth0,
                          IP4_ADDR(10, 0, 2, 15),
                          IP4_ADDR(255, 255, 255, 0),
                          IP4_ADDR(10, 0, 2, 2));

            // Register the RX callback so virtio_net_poll feeds
            // packets into the network stack
            virtio_net_set_rx_callback(vnet, virtio_rx_callback, eth0);
        }
    }

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

    kprintf("[SUMMARY] Timer: %u Hz, uptime %lu ms\n",
            TIMER_HZ, timer_get_ms());

    kprintf("[SUMMARY] TSC: %lu MHz\n", tsc_get_freq() / 1000000);

    if (eth0)
        kprintf("[SUMMARY] Network: %s  MAC=%s IP=%s\n",
                eth0->name, ETH_FMT(eth0->mac), IP4_FMT(eth0->ip_addr));
    else
        kprintf("[SUMMARY] Network: no device\n");

    kprintf("\nBoot complete.\n");

    // ── Test ping to the gateway ───────────────────────────────────────
    if (eth0)
    {
        kprintf("\n[PING] Sending echo request to gateway...\n");
        icmp_send_echo_request(IP4_ADDR(10, 0, 2, 2), 1, 1, "eMMK", 4);

        // Poll for a reply
        for (int i = 0; i < 500; ++i)
        {
            netdev_poll_all();
            netdev_process_rx();
            timer_sleep_ms(1);
        }
    }

    // ── Main poll loop ─────────────────────────────────────────────────
    kprintf("\nEntering poll loop...\n");
    for (;;)
    {
        netdev_poll_all();
        netdev_process_rx();
        __asm__ volatile("hlt");
    }
}

