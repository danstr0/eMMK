/**
 * @file virtio_net.c
 * @brief Virtio network device driver.
 *
 * @par Initialization sequence (legacy):
 * 1. Find the virtio-net PCI device.
 * 2. @c virtio_device_init() — reset, ACKNOWLEDGE, DRIVER.
 * 3. Negotiate features (MAC, STATUS).
 * 4. Create RX and TX virtqueues.
 * 5. Pre-fill the RX queue with empty buffers.
 * 6. Set up the interrupt handler.
 * 7. Set DRIVER_OK — the device is live.
 *
 * @par Transmit path
 * Allocate a page, copy @c virtio_net_hdr + packet data, add to the TX
 * queue, and kick. Completed TX buffers are freed during poll or the
 * next send.
 *
 * @par Receive path
 * Pre-filled RX buffers are completed by the device. On poll or
 * interrupt, the driver retrieves completions, invokes the RX callback
 * (if registered) or makes them available via @c virtio_net_receive(),
 * then refills the buffer slot.
 */

#include "virtio_net.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "serial.h"
#include "string.h"
#include "io.h"
#include "idt.h"
#include "apic.h"

// ── Global device instance ─────────────────────────────────────────────

/**
 * Single global virtio-net device. Used by the ISR to locate the
 * device without per-vector context. Only one NIC is supported.
 */
static struct virtio_net_dev *g_virtio_net;

// ── RX buffer management ───────────────────────────────────────────────

/**
 * @brief Refill a single RX buffer slot.
 *
 * @param dev   Virtio-net device.
 * @param virt  Virtual address of the buffer.
 * @param phys  Physical address of the buffer.
 * @return @c 0 on success, @c -1 on failure.
 */
static int refill_rx_buf(struct virtio_net_dev *dev, void *virt, uint64_t phys)
{
    memset(virt, 0, VIRTIO_NET_RX_BUF_SIZE);
    if (virtqueue_add_buf(dev->rx_vq, phys, VIRTIO_NET_RX_BUF_SIZE,
                          true, virt) < 0)
    {
        kprintf("[VIRTIO-NET] Failed to refill RX buffer\n");
        dev->rx_dropped++;
        return -1;
    }
    return 0;
}

/**
 * @brief Allocate and enqueue all RX buffers.
 *
 * Pre-fills the RX queue so the device has buffers to write received
 * packets into. Each buffer is one page (4 KiB).
 */
static int virtio_net_setup_rx_buffers(struct virtio_net_dev *dev)
{
    for (int i = 0; i < VIRTIO_NET_RX_BUFFERS; ++i)
    {
        uint64_t phys = pmm_alloc_page();
        if (phys == PMM_ALLOC_FAILED)
        {
            kprintf("[VIRTIO-NET] Failed to allocate RX buffer %d\n", i);
            return -1;
        }

        void *virt = PHYS_TO_VIRT(phys);
        memset(virt, 0, PAGE_SIZE);

        dev->rx_bufs[i].virt = virt;
        dev->rx_bufs[i].phys = phys;

        if (virtqueue_add_buf(dev->rx_vq, phys, VIRTIO_NET_RX_BUF_SIZE,
                              true, virt) < 0)
        {
            kprintf("[VIRTIO-NET] Failed to enqueue RX buffer %d\n", i);
            pmm_free_page(phys);
            return -1;
        }
    }

    virtqueue_kick(dev->rx_vq);
    kprintf("[VIRTIO-NET] Pre-filled %d RX buffers\n", VIRTIO_NET_RX_BUFFERS);
    return 0;
}

// ── TX completion reclamation ──────────────────────────────────────────

/**
 * @brief Free completed TX buffers.
 *
 * Called from @c virtio_net_poll() and before new sends to reclaim
 * pages used for transmitted packets.
 */
static void reclaim_tx_buffers(struct virtio_net_dev *dev)
{
    void *buf;
    while ((buf = virtqueue_get_buf(dev->tx_vq, NULL)) != NULL)
        pmm_free_page(VIRT_TO_PHYS_ADDR((uintptr_t)buf));
}

// ── Interrupt handler ──────────────────────────────────────────────────

void virtio_net_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;

    if (!g_virtio_net)
    {
        lapic_eoi();
        return;
    }

    // Read ISR status to acknowledge the interrupt (read clears it)
    (void)inb(g_virtio_net->vdev->io_base + VIRTIO_PCI_ISR_STATUS);

    lapic_eoi();
}

// ── Initialization ─────────────────────────────────────────────────────

struct virtio_net_dev *virtio_net_init(void)
{
    kprintf("[VIRTIO-NET] Searching for virtio-net device...\n");

    // Find the PCI device
    struct pci_device *pci =
        pci_find_device(PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET, NULL);
    if (!pci)
    {
        kprintf("[VIRTIO-NET] No virtio-net device found\n");
        return NULL;
    }

    kprintf("[VIRTIO-NET] Found at %x:%x.%x\n",
            pci->bus, pci->device, pci->function);

    // Initialize the virtio transport layer (reset + ACKNOWLEDGE + DRIVER)
    struct virtio_device *vdev = virtio_device_init(pci);
    if (!vdev)
    {
        kprintf("[VIRTIO-NET] Failed to initialize virtio transport\n");
        return NULL;
    }

    // Allocate the net device structure
    struct virtio_net_dev *dev = kzalloc(sizeof(struct virtio_net_dev));
    if (!dev)
    {
        kfree(vdev);
        return NULL;
    }
    dev->vdev = vdev;

    // Negotiate features
    uint32_t desired    = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    uint32_t negotiated = virtio_negotiate_features(vdev, desired);

    kprintf("[VIRTIO-NET] Negotiated features: 0x%lx\n",
            (uint64_t)negotiated);

    // Read the MAC address (if the feature was negotiated)
    if (negotiated & VIRTIO_NET_F_MAC)
    {
        for (int i = 0; i < 6; ++i)
            dev->mac[i] = virtio_read_config8(vdev, i);

        kprintf("[VIRTIO-NET] MAC: %x:%x:%x:%x:%x:%x\n",
                dev->mac[0], dev->mac[1], dev->mac[2],
                dev->mac[3], dev->mac[4], dev->mac[5]);
    }

    // Create RX and TX virtqueues
    // virtqueue_create() registers each queue in vdev->queues[]
    // and updates vdev->num_queues automatically
    dev->rx_vq = virtqueue_create(vdev, VIRTIO_NET_RXQ);
    if (!dev->rx_vq)
    {
        kprintf("[VIRTIO-NET] Failed to create RX queue\n");
        kfree(dev);
        kfree(vdev);
        return NULL;
    }

    dev->tx_vq = virtqueue_create(vdev, VIRTIO_NET_TXQ);
    if (!dev->tx_vq)
    {
        kprintf("[VIRTIO-NET] Failed to create TX queue\n");
        virtqueue_destroy(dev->rx_vq);
        kfree(dev);
        kfree(vdev);
        return NULL;
    }

    // Pre-fill the RX queue with empty buffers
    if (virtio_net_setup_rx_buffers(dev) < 0)
    {
        virtqueue_destroy(dev->tx_vq);
        virtqueue_destroy(dev->rx_vq);
        kfree(dev);
        kfree(vdev);
        return NULL;
    }

    // Set up the interrupt handler
    if (pci->cap_msix)
    {
        if (pci_msix_enable(pci) == 0)
        {
            pci_msix_set_entry(pci, 0, VIRTIO_NET_MSIX_VECTOR, lapic_id());
            idt_set_handler(VIRTIO_NET_MSIX_VECTOR, virtio_net_irq_handler);

            // Tell the virtio device which MSI-X entry each queue uses
            outw(vdev->io_base + VIRTIO_PCI_QUEUE_SEL, VIRTIO_NET_RXQ);
            outw(vdev->io_base + VIRTIO_PCI_QUEUE_MSIX_VECTOR, 0);
            if (inw(vdev->io_base + VIRTIO_PCI_QUEUE_MSIX_VECTOR) == VIRTIO_MSI_NO_VECTOR)
                kprintf("[VIRTIO-NET] WARNING: Device rejected RX MSI-X vector\n");

            outw(vdev->io_base + VIRTIO_PCI_QUEUE_SEL, VIRTIO_NET_TXQ);
            outw(vdev->io_base + VIRTIO_PCI_QUEUE_MSIX_VECTOR, 0);
            if (inw(vdev->io_base + VIRTIO_PCI_QUEUE_MSIX_VECTOR) == VIRTIO_MSI_NO_VECTOR)
                kprintf("[VIRTIO-NET] WARNING: Device rejected TX MSI-X vector\n");

            // Disable config-change interrupts
            outw(vdev->io_base + VIRTIO_PCI_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

            kprintf("[VIRTIO-NET] Using MSI-X vector %u\n", VIRTIO_NET_MSIX_VECTOR);
        }
    }
    else if (pci->interrupt_line < 16)
    {
        // Fall back to legacy IRQ via the I/O APIC
        uint8_t vector = IRQ_VECTOR(pci->interrupt_line);
        idt_set_handler(vector, virtio_net_irq_handler);
        ioapic_route_irq(pci->interrupt_line, vector, lapic_id());
        ioapic_unmask_irq(pci->interrupt_line);
        kprintf("[VIRTIO-NET] Using legacy IRQ %u (vector %u)\n",
                pci->interrupt_line, vector);
    }
    else
        kprintf("[VIRTIO-NET] WARNING: No interrupt configured "
                "(polling only)\n");

    // Mark the device as live
    virtio_set_status(vdev, VIRTIO_STATUS_DRIVER_OK);

    // Verify the device accepted the configuration
    uint8_t status = inb(vdev->io_base + VIRTIO_PCI_DEVICE_STATUS);
    if (status & VIRTIO_STATUS_FAILED)
    {
        kprintf("[VIRTIO-NET] ERROR: Device rejected configuration!\n");
        virtqueue_destroy(dev->tx_vq);
        virtqueue_destroy(dev->rx_vq);
        kfree(dev);
        kfree(vdev);
        return NULL;
    }

    g_virtio_net = dev;

    kprintf("[VIRTIO-NET] Initialization complete (status=0x%x)\n", status);
    return dev;
}

// ── Transmit ───────────────────────────────────────────────────────────

int virtio_net_send(struct virtio_net_dev *dev, const void *data, size_t len)
{
    if (!dev || !data || len == 0)
        return -1;

    // Reclaim any completed TX buffers first
    reclaim_tx_buffers(dev);

    if (dev->tx_vq->num_free < 1)
    {
        dev->tx_dropped++;
        return -1;
    }

    // Allocate a page for the TX buffer (header + packet)
    size_t total_len = VIRTIO_NET_HDR_SIZE + len;
    uint64_t phys = pmm_alloc_page();
    if (phys == PMM_ALLOC_FAILED)
    {
        dev->tx_dropped++;
        return -1;
    }

    void *virt = PHYS_TO_VIRT(phys);

    // Build the packet: zeroed virtio header + payload
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)virt;
    memset(hdr, 0, VIRTIO_NET_HDR_SIZE);
    memcpy((uint8_t *)virt + VIRTIO_NET_HDR_SIZE, data, len);

    // Submit to the TX queue
    if (virtqueue_add_buf(dev->tx_vq, phys, total_len, false, virt) < 0)
    {
        pmm_free_page(phys);
        dev->tx_dropped++;
        return -1;
    }

    virtqueue_kick(dev->tx_vq);

    dev->tx_packets++;
    dev->tx_bytes += len;

    return 0;
}

// ── Receive (polling, single packet) ───────────────────────────────────

int virtio_net_receive(struct virtio_net_dev *dev, void *buf, size_t buflen)
{
    if (!dev || !buf)
        return -1;

    uint32_t len;
    void *rx_buf = virtqueue_get_buf(dev->rx_vq, &len);
    if (!rx_buf)
        return 0;  // no packet available

    // Skip the virtio-net header
    if (len <= VIRTIO_NET_HDR_SIZE)
    {
        // Runt packet - refill and drop
        dev->rx_dropped++;
        refill_rx_buf(dev, rx_buf, VIRT_TO_PHYS_ADDR((uintptr_t)rx_buf));
        virtqueue_kick(dev->rx_vq);
        return 0;
    }

    const uint8_t *pkt_data = (const uint8_t *)rx_buf + VIRTIO_NET_HDR_SIZE;
    size_t pkt_len = len - VIRTIO_NET_HDR_SIZE;

    // Copy to the caller's buffer (truncate if necessary)
    size_t copy_len = (pkt_len > buflen) ? buflen : pkt_len;
    memcpy(buf, pkt_data, copy_len);

    dev->rx_packets++;
    dev->rx_bytes += pkt_len;

    // Refill the RX slot
    refill_rx_buf(dev, rx_buf, VIRT_TO_PHYS_ADDR((uintptr_t)rx_buf));
    virtqueue_kick(dev->rx_vq);

    return (int)copy_len;
}

// ── Poll (process all pending RX and TX completions) ───────────────────

void virtio_net_poll(struct virtio_net_dev *dev)
{
    if (!dev)
        return;

    // Reclaim completed TX buffers
    reclaim_tx_buffers(dev);

    // Process all completed RX buffers
    uint32_t len;
    void    *rx_buf;
    bool     refilled = false;

    while ((rx_buf = virtqueue_get_buf(dev->rx_vq, &len)) != NULL)
    {
        if (len <= VIRTIO_NET_HDR_SIZE)
        {
            dev->rx_dropped++;
            refill_rx_buf(dev, rx_buf,
                          VIRT_TO_PHYS_ADDR((uintptr_t)rx_buf));
            refilled = true;
            continue;
        }

        const uint8_t *pkt_data =
            (const uint8_t *)rx_buf + VIRTIO_NET_HDR_SIZE;
        size_t pkt_len = len - VIRTIO_NET_HDR_SIZE;

        dev->rx_packets++;
        dev->rx_bytes += pkt_len;

        // Invoke the receive callback if registered
        if (dev->rx_callback)
            dev->rx_callback(pkt_data, pkt_len, dev->rx_callback_ctx);

        // Refill this RX slot for the device to reuse
        refill_rx_buf(dev, rx_buf, VIRT_TO_PHYS_ADDR((uintptr_t)rx_buf));
        refilled = true;
    }

    // Kick only if buffers were refilled
    if (refilled)
        virtqueue_kick(dev->rx_vq);
}

// ── Callback registration ──────────────────────────────────────────────

void virtio_net_set_rx_callback(struct virtio_net_dev *dev,
                                virtio_net_rx_callback_t callback,
                                void *ctx)
{
    if (!dev)
        return;
    dev->rx_callback     = callback;
    dev->rx_callback_ctx = ctx;
}

// ── Status and configuration ───────────────────────────────────────────

void virtio_net_get_mac(struct virtio_net_dev *dev, uint8_t *mac)
{
    if (dev && mac)
        memcpy(mac, dev->mac, 6);
}

bool virtio_net_link_up(struct virtio_net_dev *dev)
{
    if (!dev)
        return false;

    // If STATUS feature wasn't negotiated, assume link is up
    if (!(dev->vdev->features & VIRTIO_NET_F_STATUS))
        return true;

    // Link status is at offset 6 in the device-specific config
    uint16_t status = virtio_read_config16(dev->vdev, 6);
    return (status & VIRTIO_NET_S_LINK_UP) != 0;
}

