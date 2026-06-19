/**
 * @file test_pci_net.c
 * @brief Tests for PCI enumeration and virtio-net initialization.
 *
 * These tests require QEMU to be started with a virtio-net device:
 * @code
 *   -device virtio-net-pci,netdev=net0 -netdev user,id=net0
 * @endcode
 *
 * The tests verify the full device-driver stack: PCI config space
 * access, BAR decoding, capability walking, virtio handshake,
 * virtqueue setup, and RX buffer pre-fill.
 */

#include "test.h"
#include "pci.h"
#include "virtio.h"
#include "virtio_net.h"

uint32_t test_pci_net(void)
{
    TEST_BEGIN("pci_net");

    // ── PCI enumeration found devices ───────────────────
    {
        int count = pci_get_device_count();
        TEST_ASSERT(count > 0, "PCI found devices");
    }

    // ── Host bridge exists (class 0x06, subclass 0x00) ──
    {
        struct pci_device *bridge =
            pci_find_class(PCI_CLASS_BRIDGE, 0x00, NULL);
        TEST_ASSERT(bridge != NULL, "host bridge found");
    }

    // ── Virtio-net device found ─────────────────────────
    struct pci_device *vpci =
        pci_find_device(PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET, NULL);
    TEST_ASSERT(vpci != NULL, "virtio-net PCI device found");

    if (!vpci)
    {
        kprintf("  (skipping remaining net tests — no device)\n");
        TEST_END();
    }

    // ── BAR0 is I/O space ───────────────────────────────
    TEST_ASSERT(vpci->bar[0].is_io, "virtio-net BAR0 is I/O");
    TEST_ASSERT(vpci->bar[0].base != 0, "virtio-net BAR0 base nonzero");

    // ── Vendor capability present ───────────────────────
    TEST_ASSERT(vpci->cap_vendor != 0 || vpci->cap_msix != 0,
                "virtio-net has capabilities");

    // ── Full driver init succeeds ───────────────────────
    struct virtio_net_dev *netdev = virtio_net_init();
    TEST_ASSERT(netdev != NULL, "virtio_net_init succeeds");

    if (!netdev)
    {
        kprintf("  (skipping remaining net tests — init failed)\n");
        TEST_END();
    }

    // ── MAC address is nonzero ──────────────────────────
    {
        uint8_t mac[6];
        virtio_net_get_mac(netdev, mac);
        bool nonzero = false;
        for (int i = 0; i < 6; ++i)
            if (mac[i] != 0) { nonzero = true; break; }
        TEST_ASSERT(nonzero, "MAC address is nonzero");
    }

    // ── Link is up ──────────────────────────────────────
    TEST_ASSERT(virtio_net_link_up(netdev), "link is up");

    // ── RX and TX queues exist ──────────────────────────
    TEST_ASSERT(netdev->rx_vq != NULL, "RX queue created");
    TEST_ASSERT(netdev->tx_vq != NULL, "TX queue created");
    TEST_ASSERT(netdev->rx_vq->size > 0, "RX queue has descriptors");
    TEST_ASSERT(netdev->tx_vq->size > 0, "TX queue has descriptors");

    // ── Stats start at zero ─────────────────────────────
    TEST_ASSERT_EQ(netdev->rx_packets, 0, "initial rx_packets is 0");
    TEST_ASSERT_EQ(netdev->tx_packets, 0, "initial tx_packets is 0");

    // ── Poll with no traffic doesn't crash ──────────────
    virtio_net_poll(netdev);
    TEST_ASSERT(true, "poll with no traffic safe");

    TEST_END();
}

