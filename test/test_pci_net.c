/**
 * @file test_pci_net.c
 * @brief Tests for PCI enumeration, the driver model, MSI-X vector
 *        allocation, and virtio-net probing of every NIC.
 *
 * QEMU is started with one or two virtio-net devices
 * (`make test NICs=1|2`):
 * @code
 *   -device virtio-net-pci,netdev=net0 -netdev user,id=net0
 *   -device virtio-net-pci,netdev=net1 -netdev user,id=net1,net=10.0.3.0/24
 * @endcode
 */

#include "test.h"
#include "pci.h"
#include "irq.h"
#include "virtio.h"
#include "virtio_net.h"
#include "netdev.h"
#include "netbuf.h"
#include "ethernet.h"
#include "io.h"
#include "icmp.h"
#include "timer.h"
#include "format.h"

// ── Test drivers ───────────────────────────────────────────────────────

static int probe_calls;

static int probe_decline(struct pci_device *dev)
{
    (void)dev;
    probe_calls++;
    return -1;
}

static int probe_accept(struct pci_device *dev)
{
    (void)dev;
    probe_calls++;
    return 0;
}

static const struct pci_device_id virtio_net_only[] =
{
    { PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET },
    { PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET_MODERN },
    { 0, 0 },
};

static const struct pci_device_id any_device[] =
{
    { PCI_ANY_ID, PCI_ANY_ID },
    { 0, 0 },
};

/// Filled in at run time with the host bridge's IDs.
static struct pci_device_id bridge_ids[2];

static const struct pci_driver drv_virtio_dup =
    { .name = "test-virtio", .ids = virtio_net_only, .probe = probe_accept };
static const struct pci_driver drv_decline_all =
    { .name = "test-decline", .ids = any_device, .probe = probe_decline };
static const struct pci_driver drv_bridge =
    { .name = "test-bridge", .ids = bridge_ids, .probe = probe_accept };
static const struct pci_driver drv_no_probe = 
    { .name = "test-invalid", .ids = any_device, .probe = NULL };

/// @return @c true if @p x is a power of two.
static bool is_pow2(uint64_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/// Send one echo request to each NIC's gateway.
static void send_pings(int nics, uint16_t seq)
{
    for (int i = 0; i < nics; ++i)
        icmp_send_echo_request(IP4_ADDR(10, 0, 2 + i, 2),
                               0x5b, seq, "5b", 2);
}

uint32_t test_pci_net(void)
{
    TEST_BEGIN("pci_net");

    // ── PCI enumeration found devices ───────────────────
    int count = pci_get_device_count();
    TEST_ASSERT(count > 0, "PCI found devices");

    // ── Host bridge exists (class 0x06, subclass 0x00) ──
    struct pci_device *bridge =
        pci_find_class(PCI_CLASS_BRIDGE, 0x00, NULL);
    TEST_ASSERT(bridge != NULL, "host bridge found");

    // ── Every virtio-net function was probed ────────────
    // Transitional (QEMU's default) and modern-only
    int nfound = 0, nmodern = 0;
    for (struct pci_device *p = NULL;
         (p = pci_find_device(PCI_VENDOR_VIRTIO,
                              VIRTIO_DEV_NET, p)) != NULL; )
        nfound++;
    for (struct pci_device *p = NULL;
         (p = pci_find_device(PCI_VENDOR_VIRTIO,
                              VIRTIO_DEV_NET_MODERN, p)) != NULL; )
        nmodern++;
    nfound += nmodern;
    if (nmodern)
        kprintf("  %d modern-only device(s)\n", nmodern);
    TEST_ASSERT(nfound >= 1, "virtio-net PCI device found");
    TEST_ASSERT_EQ(virtio_net_count(), nfound,
                   "every virtio-net device probed (not just the first)");

    if (virtio_net_count() == 0)
    {
        kprintf("  (skipping remaining net tests — no device)\n");
        TEST_END();
    }

    if (nfound < 2)
        kprintf("  single NIC: multi-device checks are trivial "
                "(NIC=2 for more)\n");

    // ── Per-device state ────────────────────────────────
    bool bound_ok = true, mac_ok = true, link_ok = true, queues_ok = true;
    bool netdev_ok = true, vector_ok = true, vecreg_ok = true, stats_ok = true;
    bool regions_ok = true, features_ok = true, status_ok = true;
    for (int i = 0; i < virtio_net_count(); ++i)
    {
        struct virtio_net_dev *dev = virtio_net_get(i);
        struct pci_device *pci = dev->pci;
        struct virtio_device *vdev = dev->vdev;
    
        if (!pci->driver || pci->driver_data != dev || dev->index != i)
            bound_ok = false;

        // Modern transport: every register region mapped
        if (!vdev->common || !vdev->notify || !vdev->isr || !vdev->device_cfg
            || vdev->device_cfg_len < 8 || vdev->notify_off_multiplier == 0
            || vdev->max_queues < 2)
            regions_ok = false;

        // VERSION_1 always; MAC and STATUS are offered by QEMU
        uint64_t want = VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
        if ((vdev->features & want) != want)
            features_ok = false;

        uint8_t st = virtio_get_status(vdev);
        uint8_t live = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
                     | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
        if ((st & live) != live
            || (st & (VIRTIO_STATUS_FAILED | VIRTIO_STATUS_NEEDS_RESET)))
            status_ok = false;

        // QEMU assigns 52:52:00:xx:xx:xx
        if (dev->mac[0] != 0x52
         || dev->mac[1] != 0x54
         || dev->mac[2] != 0x00)
            mac_ok = false;

        if (!virtio_net_link_up(dev))
            link_ok = false;

        if (!dev->rx_vq || !dev->tx_vq || dev->rx_vq->size == 0
            || dev->tx_vq->size == 0)
            queues_ok = false;
    
        // Its own netdev, named in probe order, carrying its MAC
        char name[16];
        ksnprintf(name, sizeof(name), "eth%d", i);
        struct netdev *nd = dev->netdev;
        if (!nd || strcmp(nd->name, name) != 0 || nd->priv != dev
            || netdev_find(name) != nd || !netdev_is_up(nd)
            || memcmp(nd->mac, dev->mac, 6) != 0)
            netdev_ok = false;

        // An allocated vector that leads back to this device
        if (dev->irq_vector < IRQ_DYN_FIRST
         || dev->irq_vector > IRQ_DYN_LAST)
            vector_ok = false;
        else if (dev->intx_line >= 0)
        {
            kprintf("  %s uses INTx line %d\n", dev->netdev->name,
                    dev->intx_line);
            if (irq_vector_ctx(dev->irq_vector) != (void *)(uintptr_t)
                                                   (dev->intx_line + 1))
                vector_ok = false;
        }
        else
        {
            if (pci->msix_allocated != 1
                || pci->msix_vectors[0] != dev->irq_vector
                || irq_vector_ctx(dev->irq_vector) != dev)
                vector_ok = false;

            // Both queues signal MSI-X entry 0
            volatile uint8_t *c = vdev->common;
            mmio_write16(c + VIRTIO_COMMON_Q_SELECT, VIRTIO_NET_RXQ);
            uint16_t rxv = mmio_read16(c + VIRTIO_COMMON_Q_MSIX);
            uint16_t rxe = mmio_read16(c + VIRTIO_COMMON_Q_ENABLE);
            mmio_write16(c + VIRTIO_COMMON_Q_SELECT, VIRTIO_NET_TXQ);
            uint16_t txv = mmio_read16(c + VIRTIO_COMMON_Q_MSIX);
            uint16_t txe = mmio_read16(c + VIRTIO_COMMON_Q_ENABLE);
            if (rxv != 0 || txv != 0 || rxe != 1 || txe != 1
                || mmio_read16(c + VIRTIO_COMMON_MSIX) != VIRTIO_MSI_NO_VECTOR)
                vecreg_ok = false;
        }

        if (dev->netdev->stats.rx_packets != 0
            || dev->netdev->stats.tx_packets != 0)
            stats_ok = false;
    }
    TEST_ASSERT(bound_ok, "each device bound");
    TEST_ASSERT(mac_ok, "MACs have QEMU's 52:54:00 prefix");
    TEST_ASSERT(link_ok, "links are up");
    TEST_ASSERT(queues_ok, "RX and TX queues created with descriptors");
    TEST_ASSERT(netdev_ok, "each device has its own netdev ethN");
    TEST_ASSERT(vector_ok, "each device has an allocated MSI-X vector");
    TEST_ASSERT(vecreg_ok, "queues enabled on MSI-X entry 0");
    TEST_ASSERT(regions_ok, "common/notify/ISR/device regions mapped");
    TEST_ASSERT(features_ok, "VERSION_1, MAC, and STATUS negotiated");
    TEST_ASSERT(status_ok, "status: FEATURES_OK and DRIVER_OK, no FAILED");
    TEST_ASSERT(stats_ok, "stats start at zero");

    // ── Distinct MACs and vectors across devices ────────
    bool distinct = true;
    for (int i = 0; i < virtio_net_count(); ++i)
        for (int j = i + 1; j < virtio_net_count(); ++j)
        {
            struct virtio_net_dev *a = virtio_net_get(i);
            struct virtio_net_dev *b = virtio_net_get(j);
            if (memcmp(a->mac, b->mac, 6) == 0
                || a->irq_vector == b->irq_vector
                || a->netdev == b->netdev)
                distinct = false;
        }
    TEST_ASSERT(distinct, "devices have distinct MACs, "
                          "vectors, and netdevs");
    TEST_ASSERT_EQ(irq_vectors_free(), IRQ_DYN_COUNT - virtio_net_count(),
                   "one vector allocated per device");

    struct virtio_net_dev *first = virtio_net_get(0);
    struct pci_device *vpci = first->pci;

    // ── BARs and pci_map_bar() ──────────────────────────
    {
        // A transitional device still has its legacy I/O BAR (unused);
        // a modern-only one has none
        if (vpci->device_id == VIRTIO_DEV_NET)
        {
            TEST_ASSERT(vpci->bar[0].is_io && vpci->bar[0].base != 0,
                        "transitional virtio-net BAR0 is I/O");
            TEST_ASSERT(pci_map_bar(vpci, 0) == NULL,
                        "I/O BAR is not mapped");
        }
        else
        {
            bool no_io = true;
            for (int b = 0; b < 6; ++b)
                if (vpci->bar[b].size && vpci->bar[b].is_io)
                    no_io = false;
            TEST_ASSERT(no_io, "modern-only virtio-net has no I/O BAR");
        }
        TEST_ASSERT(pci_map_bar(vpci, 6) == NULL, "BAR index 6 rejected");
    
        bool sizes_ok = true, upper_ok = true;
        int mem_bars = 0;
        for (int b = 0; b < 6; ++b)
        {
            if (vpci->bar[b].size == 0)
                continue;
            if (!is_pow2(vpci->bar[b].size))
                sizes_ok = false;
            if (vpci->bar[b].is_io)
                continue;

            mem_bars++;
            void *v1 = pci_map_bar(vpci, b);
            void *v2 = pci_map_bar(vpci, b);
            if (!v1 || v1 != v2)
                sizes_ok = false;
        
            // Upper half of a 64-bit BAR is not a BAR of its own
            if (vpci->bar[b].is_64bit && pci_map_bar(vpci, b + 1) != NULL)
                upper_ok = false;
        }
        TEST_ASSERT(mem_bars > 0, "virtio-net has memory BARs");
        TEST_ASSERT(sizes_ok, "BAR sizes are powers of two; "
                              "mapping is cached");
        TEST_ASSERT(upper_ok, "upper half of a 64-bit BAR is not mappable");
    
        // MSI-X table is within its BAR's mapping
        uint32_t info =
            pci_device_read32(vpci, vpci->cap_msix + MSIX_TABLE_OFFSET);
        uint8_t *bar_virt = pci_map_bar(vpci, info & MSIX_BAR_MASK);
        TEST_ASSERT(bar_virt && (uint8_t *)vpci->msix_table
                                == bar_virt + (info & MSIX_OFFSET_MASK),
                    "MSI-X table mapped through pci_map_bar()");
    }

    // ── Capability iteration ────────────────────────────
    {
        int vendor_caps = 0;
        unsigned types_seen = 0;
        uint8_t c;
        for (c = pci_find_next_capability(vpci, PCI_CAP_VENDOR, 0); c;
             c = pci_find_next_capability(vpci, PCI_CAP_VENDOR, c))
        {
            vendor_caps++;
            uint8_t cfg_type = pci_device_read8(vpci, c + 3);
            if (cfg_type < 32)
                types_seen |= 1U << cfg_type;
        }

        // A transitional virtio device carries one vendor capability per
        // modern register region
        TEST_ASSERT(vendor_caps >= 4, "all vendor capabilities found");
        unsigned want = (1U << VIRTIO_PCI_CAP_COMMON_CFG)
                      | (1U << VIRTIO_PCI_CAP_NOTIFY_CFG)
                      | (1U << VIRTIO_PCI_CAP_ISR_CFG)
                      | (1U << VIRTIO_PCI_CAP_DEVICE_CFG);
        TEST_ASSERT((types_seen & want) == want,
                    "virtio common/notify/ISR/device capabilities present");
        TEST_ASSERT_EQ(pci_find_next_capability(vpci, PCI_CAP_VENDOR, 0),
                       vpci->cap_vendor, "first instance = cap_vendor");
        TEST_ASSERT_EQ(pci_find_next_capability(vpci, PCI_CAP_MSIX, 0),
                       vpci->cap_msix, "MSI-X capability found");
        TEST_ASSERT_EQ(pci_find_next_capability(vpci, PCI_CAP_MSIX,
                       vpci->cap_msix), 0,
                       "only one MSI-X capability");
    }

    // ── MSI-X allocation limits ─────────────────────────
    TEST_ASSERT_EQ(pci_msix_alloc(vpci, 1, virtio_net_irq_handler, first),
                   -1, "second pci_msix_alloc on a device fails");
    TEST_ASSERT_EQ(pci_msix_alloc(bridge, 1, virtio_net_irq_handler, NULL),
                   -1, "pci_msix_alloc without MSI-X fails");
    TEST_ASSERT_EQ(irq_vectors_free(), IRQ_DYN_COUNT - virtio_net_count(),
                   "failed allocations leak no vectors");

    // ── Driver registry ─────────────────────────────────
    {
        TEST_ASSERT(pci_match_id(virtio_net_only, vpci), "ID table matches");
        TEST_ASSERT(!pci_match_id(virtio_net_only, bridge),
                    "ID table mismatch");
        TEST_ASSERT(pci_match_id(any_device, bridge), "wildcard matches");

        TEST_ASSERT_EQ(pci_register_driver(&drv_no_probe), -1,
                       "driver without probe rejected");

        // Late registration sees only unbound devices: none for virtio-net
        probe_calls = 0;
        TEST_ASSERT_EQ(pci_register_driver(&drv_virtio_dup), 0,
                       "late driver binds nothing already bound");
        TEST_ASSERT_EQ(probe_calls, 0, "bound devices are not re-probed");
        TEST_ASSERT_EQ(pci_register_driver(&drv_virtio_dup), -1,
                       "duplicate registration rejected");
        pci_unregister_driver(&drv_virtio_dup);
    
        // A catch-all driver that declines is offered every unbound device
        int unbound = 0;
        for (int i = 0; i < count; ++i)
            if (!pci_get_device(i)->driver)
                unbound++;
        probe_calls = 0;
        TEST_ASSERT_EQ(pci_register_driver(&drv_decline_all), 0,
                       "declining driver binds nothing");
        TEST_ASSERT_EQ(probe_calls, unbound,
                       "probe offered every unbound device");
        bool still_unbound = true;
        for (int i = 0; i < count; ++i)
            if (pci_get_device(i)->driver == &drv_decline_all)
                still_unbound = false;
        TEST_ASSERT(still_unbound, "declined devices stay unbound");
        pci_unregister_driver(&drv_decline_all);
    
        // An accepting driver binds exactly its matches, and unbinds on
        // unregister
        bridge_ids[0].vendor = bridge->vendor_id;
        bridge_ids[0].device = bridge->device_id;
        int matches = 0;
        for (int i = 0; i < count; ++i)
            if (!pci_get_device(i)->driver
                && pci_match_id(bridge_ids, pci_get_device(i)))
                matches++;
        probe_calls = 0;
        TEST_ASSERT_EQ(pci_register_driver(&drv_bridge), matches,
                       "accepting driver binds its matches");
        TEST_ASSERT(bridge->driver == &drv_bridge, "host bridge bound");
        pci_unregister_driver(&drv_bridge);
        TEST_ASSERT(bridge->driver == NULL, "unregister unbinds");
        TEST_ASSERT(vpci->driver != NULL, "other bridges untouched");
    }

    // ── End-to-end: every NIC pings its own gateway ─────
    {
        int n = virtio_net_count();
        uint64_t irq_before[VIRTIO_NET_MAX_DEVICES];
        for (int i = 0; i < n; ++i)
        {
            struct virtio_net_dev *dev = virtio_net_get(i);
            netdev_set_ip(dev->netdev, IP4_ADDR(10, 0, 2 + i, 15),
                          IP4_ADDR(255, 255, 255, 0),
                          IP4_ADDR(10, 0, 2 + i, 2));
            irq_before[i] = dev->irq_count;
        }

        struct icmp_stats s0, s1;
        __asm__ volatile("sti");

        icmp_get_stats(&s0);
        send_pings(n, 1);

        bool zero_copy = true;
        uint64_t deadline = timer_deadline_ms(3000);
        do
        {
            for (int i = 0; i < n; ++i)
            {
                struct virtio_net_dev *dev = virtio_net_get(i);
                struct netbuf *pkts[NETDEV_BURST_MAX];
                uint16_t got = netdev_rx_burst(dev->netdev, 0, pkts,
                                               NETDEV_BURST_MAX);
                for (uint16_t j = 0; j < got; ++j)
                {
                    if (pkts[j]->pool != dev->rx_pool
                     || pkts[j]->dev != dev->netdev
                     || pkts[j]->data != pkts[j]->head)
                     zero_copy = false;
                    eth_input(pkts[j]);
                }
            }
            icmp_get_stats(&s1);
        } while (s1.rx_echo_reply - s0.rx_echo_reply < (uint64_t)n
                 && !timer_deadline_expired(deadline));

        TEST_ASSERT_EQ(s1.rx_echo_reply - s0.rx_echo_reply, (uint64_t)n,
                       "every NIC's gateway answers ping");
        TEST_ASSERT(zero_copy, "received netbufs are the pool's DMA buffers");

        bool rx_ok = true, quiet = true;
        for (int i = 0; i < n; ++i)
        {
            if (virtio_net_get(i)->netdev->stats.rx_packets == 0)
                rx_ok = false;
            if (virtio_net_get(i)->irq_count != irq_before[i])
                quiet = false;
        }
        TEST_ASSERT(rx_ok, "replies arrived on each NIC's own netdev");
        TEST_ASSERT(quiet, "polled queues raise no interrupt");

        // Interrupts allowed: every NIC raises its own
        netdev_rx_irq_all(true);
        icmp_get_stats(&s0);
        send_pings(n, 2);

        bool irqs_ok = false;
        deadline = timer_deadline_ms(3000);
        do
        {
            netdev_poll_all();
            icmp_get_stats(&s1);

            irqs_ok = true;
            for (int i = 0; i < n; ++i)
                if (virtio_net_get(i)->irq_count == irq_before[i])
                    irqs_ok = false;
        } while ((s1.rx_echo_reply - s0.rx_echo_reply < (uint64_t)n || !irqs_ok)
                 && !timer_deadline_expired(deadline));
        netdev_rx_irq_all(false);

        TEST_ASSERT(irqs_ok, "every NIC raised its own interrupt");

        // Every buffer accounter for: on the ring, or free in the pool
        bool ring_full = true, no_leak = true;
        for (int i = 0; i < n; ++i)
        {
            struct virtio_net_dev *dev = virtio_net_get(i);
            netdev_tx_burst(dev->netdev, 0, NULL, 0);

            uint32_t posted = dev->rx_vq->size - dev->rx_vq->num_free;
            if (dev->rx_vq->num_free != 0)
                ring_full = false;
            if (netbuf_pool_available(dev->rx_pool) + posted
                != dev->rx_pool->capacity)
                no_leak = false;
        }
        TEST_ASSERT(ring_full, "RX ring refilled completely");
        TEST_ASSERT(no_leak, "no RX buffer leaked");

        // Unconfigure so later suites route only to their fake devices
        for (int i = 0; i < n; ++i)
            netdev_set_ip(virtio_net_get(i)->netdev, 0, 0, 0);
    }

    TEST_END();
}

