/**
 * @file virtio_net.c
 * @brief Virtio network device driver.
 *
 * @par Probe sequence (legacy transport), once per virtio-net device:
 * 1. @c virtio_device_init() — reset, ACKNOWLEDGE, DRIVER.
 * 2. Negotiate features (MAC, STATUS) and read the MAC.
 * 3. Create RX and TX virtqueues.
 * 4. Pre-fill the RX queue with empty buffers.
 * 5. Interrupt: one MSI-X entry with a vector from the kernel allocator
 *    (or the shared legacy INTx line as a fallback).
 * 6. Set DRIVER_OK — the device is live.
 * 7. Register a @c netdev (@c eth0, @c eth1, ...) for it.
 *
 * @par Transmit path
 * Allocate a page, copy @c virtio_net_hdr + packet data, add to the TX
 * queue, and kick. Completed TX buffers are freed during poll or the
 * next send.
 *
 * @par Receive path
 * Pre-filled RX buffers are completed by the device. On poll, the driver
 * copies each frame into a @c netbuf, hands it to @c netdev_receive(),
 * and refills the buffer slot.
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
#include "irq.h"
#include "apic.h"
#include "netbuf.h"
#include "netdev.h"
#include "ethernet.h"

// ── Instance table ─────────────────────────────────────────────────────

/// Every probed device, in probe order.
static struct virtio_net_dev *instances[VIRTIO_NET_MAX_DEVICES];
static int instance_count = 0;

/// Vector allocated for each legacy INTx line (0 = none yet). Several
/// devices may share one line; its handler acknowledges all of them.
static uint8_t intx_vectors[16];

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
        dev->rx_buf_count    = i + 1;

        if (virtqueue_add_buf(dev->rx_vq, phys, VIRTIO_NET_RX_BUF_SIZE,
                              true, virt) < 0)
        {
            kprintf("[VIRTIO-NET] Failed to enqueue RX buffer %d\n", i);
            return -1;
        }
    }

    virtqueue_kick(dev->rx_vq);
    kprintf("[VIRTIO-NET] Pre-filled %d RX buffers\n", VIRTIO_NET_RX_BUFFERS);
    return 0;
}

/// Free the RX buffer pages (probe failure path; device is reset).
static void free_rx_buffers(struct virtio_net_dev *dev)
{
    for (int i = 0; i < dev->rx_buf_count; ++i)
        pmm_free_page(dev->rx_bufs[i].phys);
    dev->rx_buf_count = 0;
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

// ── Interrupt handlers ─────────────────────────────────────────────────

void virtio_net_irq_handler(struct interrupt_frame *frame)
{
    struct virtio_net_dev *dev = irq_vector_ctx(frame->vector);

    // Read ISR status to acknowledge the interrupt (read clears it)
    if (dev)
    {
        (void)inb(dev->vdev->io_base + VIRTIO_PCI_ISR_STATUS);
        dev->irq_count++;
    }

    lapic_eoi();
}

/**
 * @brief Handler for a legacy INTx line, possibly shared by several
 *        virtio-net devices: acknowledges every device on the line.
 *
 * The vector's context is the line number + 1 (so line 0 is not NULL).
 */
static void virtio_net_intx_handler(struct interrupt_frame *frame)
{
    int line = (int)(uintptr_t)irq_vector_ctx(frame->vector) - 1;

    for (int i = 0; i < VIRTIO_NET_MAX_DEVICES; ++i)
    {
        struct virtio_net_dev *dev = instances[i];
        if (dev && dev->intx_line == line
            && inb(dev->vdev->io_base + VIRTIO_PCI_ISR_STATUS))
            dev->irq_count++;
    }

    lapic_eoi();
}

/**
 * @brief Route both virtqueues to one MSI-X entry with its own vector.
 * @return @c 0 on success, @c -1 if MSI-X could not be set up.
 */
static int setup_msix(struct virtio_net_dev *dev)
{
    struct pci_device *pci = dev->pci;
    struct virtio_device *vdev = dev->vdev;

    if (pci_msix_alloc(pci, 1, virtio_net_irq_handler, dev) < 0)
        return -1;

    // Device config now sits after the MSI-X vector registers
    vdev->config_offset = VIRTIO_PCI_CONFIG_MSIX;

    // Tell the virtio device which MSI-X entry each queue uses
    static const uint16_t queues[] = { VIRTIO_NET_RXQ, VIRTIO_NET_TXQ };
    for (int i = 0; i < 2; ++i)
    {
        outw(vdev->io_base + VIRTIO_PCI_QUEUE_SEL, queues[i]);
        outw(vdev->io_base + VIRTIO_PCI_QUEUE_MSIX_VECTOR, 0);
        if (inw(vdev->io_base + VIRTIO_PCI_QUEUE_MSIX_VECTOR)
            == VIRTIO_MSI_NO_VECTOR)
            kprintf("[VIRTIO-NET] WARNING: Device rejected queue %u "
                    "MSI-X entry\n", queues[i]);
    }

    // Disable config-change interrupts
    outw(vdev->io_base + VIRTIO_PCI_CONFIG_MSIX_VECTOR,
         VIRTIO_MSI_NO_VECTOR);

    dev->irq_vector = pci->msix_vectors[0];
    kprintf("[VIRTIO-NET] Using MSI-X vector %u\n", dev->irq_vector);
    return 0;
}

/**
 * @brief Fall back to the device's legacy INTx line via the I/O APIC.
 * @return @c 0 on success, @c -1 if there is no usable line.
 */
static int setup_intx(struct virtio_net_dev *dev)
{
    uint8_t line = dev->pci->interrupt_line;
    if (line >= 16)
        return -1;

    if (intx_vectors[line] == 0)
    {
        int vector = irq_alloc_vector(virtio_net_intx_handler,
                                      (void *)(uintptr_t)(line + 1));
        if (vector < 0)
            return -1;
        intx_vectors[line] = (uint8_t)vector;
    
        // TODO: revisit with real-hardware update
        ioapic_route_irq(line, (uint8_t)vector, lapic_id());
        ioapic_unmask_irq(line);
    }

    dev->intx_line = line;
    dev->irq_vector = intx_vectors[line];
    kprintf("[VIRTIO-NET] Using legacy IRQ %u (vector %u)\n",
            line, dev->irq_vector);
    return 0;
}

// ── netdev glue ────────────────────────────────────────────────────────

/**
 * @brief @c netdev_ops.send: transmit @p nb on the device's TX queue.
 *
 * @c virtio_net_send() copies the frame into its own DMA buffer, so the
 * @c netbuf is freed here. The driver always takes ownership of @p nb.
 */
static int virtio_netdev_send(struct netdev *ndev, struct netbuf *nb)
{
    struct virtio_net_dev *dev = ndev->priv;
    int ret = virtio_net_send(dev, nb->data, nb->len);
    netbuf_free(nb);
    return ret;
}

/// @c netdev_ops.poll: deliver completed RX frames to the stack.
static void virtio_netdev_poll(struct netdev *ndev)
{
    virtio_net_poll(ndev->priv);
}

static const struct netdev_ops virtio_netdev_ops =
{
    .send = virtio_netdev_send,
    .poll = virtio_netdev_poll,
};

/// Create, register, and bring up the netdev for @p dev.
static int register_netdev(struct virtio_net_dev *dev)
{
    // NULL name: netdev_register() picks the lowest free ethN
    struct netdev *ndev = netdev_alloc(NULL);
    if (!ndev)
        return -1;

    memcpy(ndev->mac, dev->mac, ETH_ALEN);
    ndev->ops = &virtio_netdev_ops;
    ndev->priv = dev;

    if (netdev_register(ndev) < 0)
    {
        kfree(ndev);
        return -1;
    }
    netdev_set_up(ndev);

    dev->netdev = ndev;
    return 0;
}

// ── Probe ──────────────────────────────────────────────────────────────

/**
 * @brief @c pci_driver.probe: bring up one virtio-net device.
 *
 * On failure the device is reset (so it stops DMA into buffers that are
 * then freed) and everything allocated here is released.
 */
static int virtio_net_probe(struct pci_device *pci)
{
    if (instance_count >= VIRTIO_NET_MAX_DEVICES)
    {
        kprintf("[VIRTIO-NET] Too many devices, ignoring %x:%x.%x\n",
                pci->bus, pci->device, pci->function);
        return -1;
    }

    kprintf("[VIRTIO-NET] Probing %x:%x.%x\n",
            pci->bus, pci->device, pci->function);

    // Initialize the virtio transport layer (reset + ACKNOWLEDGE + DRIVER)
    struct virtio_device *vdev = virtio_device_init(pci);
    if (!vdev)
    {
        kprintf("[VIRTIO-NET] Failed to initialize virtio transport\n");
        return -1;
    }

    struct virtio_net_dev *dev = kzalloc(sizeof(struct virtio_net_dev));
    if (!dev)
    {
        kfree(vdev);
        return -1;
    }
    dev->vdev       = vdev;
    dev->pci        = pci;
    dev->intx_line  = -1;
    dev->irq_vector = 0;

    // Negotiate features
    uint32_t desired = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    uint32_t negotiated = virtio_negotiate_features(vdev, desired);

    // Read the MAC address (if the feature was negotiated).
    if (negotiated & VIRTIO_NET_F_MAC)
        for (int i = 0; i < 6; ++i)
            dev->mac[i] = virtio_read_config8(vdev, i);

    kprintf("[VIRTIO-NET] MAC: %s\n", ETH_FMT(dev->mac));

    // Create RX and TX virtqueues (registered in vdev->queues[])
    dev->rx_vq = virtqueue_create(vdev, VIRTIO_NET_RXQ);
    dev->tx_vq = dev->rx_vq ? virtqueue_create(vdev, VIRTIO_NET_TXQ)
                            : NULL;
    if (!dev->tx_vq)
    {
        kprintf("[VIRTIO-NET] Failed to create virtqueues\n");
        goto fail;
    }

    if (virtio_net_setup_rx_buffers(dev) < 0)
        goto fail;

    // Claim the slot before interrupts can fire
    instances[instance_count] = dev;

    // Interrupt: MSI-X with an allocated vector, else legacy INTx
    if (!(pci->cap_msix && setup_msix(dev) == 0) && setup_intx(dev) < 0)
        kprintf("[VIRTIO-NET] WARNING: No interrupt configured "
                "(polling only)\n");

    // Mark the device as live
    virtio_set_status(vdev, VIRTIO_STATUS_DRIVER_OK);

    // Verify the device accepted the configuration
    uint8_t status = inb(vdev->io_base + VIRTIO_PCI_DEVICE_STATUS);
    if (status & VIRTIO_STATUS_FAILED)
    {
        kprintf("[VIRTIO-NET] ERROR: Device rejected configuration\n");
        goto fail;
    }

    if (register_netdev(dev) < 0)
        goto fail;

    dev->index = instance_count++;
    pci->driver_data = dev;

    kprintf("[VIRTIO-NET] %s ready (status=0x%x)\n",
            dev->netdev->name, status);
    return 0;

fail:
    // Stop the device first: it may own RX buffers and ring memory
    virtio_reset(vdev);
    instances[instance_count] = NULL;
    if (dev->intx_line < 0)
        pci_msix_free(pci);  // INTx vectors stay with their line
    vdev->config_offset = VIRTIO_PCI_CONFIG;
    free_rx_buffers(dev);
    if (dev->tx_vq)
        virtqueue_destroy(dev->tx_vq);
    if (dev->rx_vq)
        virtqueue_destroy(dev->rx_vq);
    kfree(dev);
    kfree(vdev);
    return -1;
}

static const struct pci_device_id virtio_net_ids[] =
{
    { PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET },
    { 0, 0 },
};

static const struct pci_driver virtio_net_driver =
{
    .name  = "virtio-net",
    .ids   = virtio_net_ids,
    .probe = virtio_net_probe,
};

int virtio_net_register(void)
{
    return pci_register_driver(&virtio_net_driver);
}

// ── Instance access ────────────────────────────────────────────────────

int virtio_net_count(void)
{
    return instance_count;
}

struct virtio_net_dev *virtio_net_get(int index)
{
    if (index < 0 || index >= instance_count)
        return NULL;
    return instances[index];
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

/**
 * @brief Copy one received frame into a @c netbuf and queue it on the
 *        device's netdev (TODO: will soon be replaced with zero-copy RX).
 */
static void deliver_rx(struct virtio_net_dev *dev, const void *data,
                       size_t len)
{
    if (!dev->netdev)
        return;

    struct netbuf *nb = netbuf_alloc(0);
    if (!nb)
    {
        dev->rx_dropped++;
        return;
    }

    void *p = netbuf_put(nb, len);
    if (!p)
    {
        netbuf_free(nb);
        dev->rx_dropped++;
        return;
    }
    memcpy(p, data, len);

    netdev_receive(dev->netdev, nb);
}

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
            dev->rx_dropped++;
        else
        {
            size_t pkt_len = len - VIRTIO_NET_HDR_SIZE;
            dev->rx_packets++;
            dev->rx_bytes += pkt_len;
            deliver_rx(dev, (const uint8_t *)rx_buf + VIRTIO_NET_HDR_SIZE,
                       pkt_len);
        }

        // Refill this RX slot for the device to reuse
        refill_rx_buf(dev, rx_buf, VIRT_TO_PHYS_ADDR((uintptr_t)rx_buf));
        refilled = true;
    }

    // Kick only if buffers were refilled
    if (refilled)
        virtqueue_kick(dev->rx_vq);
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

