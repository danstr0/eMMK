/**
 * @file virtio_net.c
 * @brief Virtio network device driver.
 *
 * @par Probe sequence (modern transport), once per virtio-net device:
 * 1. @c virtio_device_init() — map the register regions, reset,
 *    ACKNOWLEDGE, DRIVER.
 * 2. Negotiate features (VERSION_1, MAC, STATUS) with FEATURES_OK, and
 *    read the MAC.
 * 3. Interrupt: one MSI-X entry with a vector from the kernel allocator
 *    (or the shared INTx line as a fallback). Before the queues: each
 *    queue's MSI-X entry is programmed when it is created.
 * 4. Create RX and TX virtqueues, suppress their interrupts, create the
 *    RX queue's netbuf pool, and fill the ring from it.
 * 5. Set DRIVER_OK — the device is live.
 * 6. Register a @c netdev (@c eth0, @c eth1, ...) for it.
 *
 * @par Transmit path
 * @c tx_burst writes a zeroed @c virtio_net_hdr into the headroom just
 * below the frame, posts the netbuf itself, and rings one doorbell.
 * Completed netbufs are freed by the next @c tx_burst.
 *
 * @par Receive path
 * The ring holds netbufs from the queue's pool; the device writes header
 * + frame straight into them. @c rx_burst hands those same netbufs to the
 * caller and refills the ring from the pool with one doorbell.
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
#include "smp.h"

// ── Instance table ─────────────────────────────────────────────────────

/// Every probed device, in probe order.
static struct virtio_net_dev *instances[VIRTIO_NET_MAX_DEVICES];
static int instance_count = 0;

/// Vector allocated for each INTx line (0 = none yet). Several devices
/// may share one line; its handler acknowledges all of them.
static uint8_t intx_vectors[16];

// ── Interrupt handlers ─────────────────────────────────────────────────

void virtio_net_irq_handler(struct interrupt_frame *frame)
{
    struct virtio_net_dev *dev = irq_vector_ctx(frame->vector);

    // With MSI-X the device does not use the ISR status register; there
    // is nothing to acknowledge at the device
    if (dev)
        dev->irq_count++;

    lapic_eoi();
}

/**
 * @brief Handler for an INTx line, possibly shared by several virtio-net
 *        devices: acknowledges every device on the line (reading the ISR
 *        status clears it and deasserts the line).
 *
 * The vector's context is the line number + 1 (so line 0 is not NULL).
 */
static void virtio_net_intx_handler(struct interrupt_frame *frame)
{
    int line = (int)(uintptr_t)irq_vector_ctx(frame->vector) - 1;

    for (int i = 0; i < VIRTIO_NET_MAX_DEVICES; ++i)
    {
        struct virtio_net_dev *dev = instances[i];
        if (dev && dev->intx_line == line && virtio_read_isr(dev->vdev))
            dev->irq_count++;
    }

    lapic_eoi();
}

/**
 * @brief Allocate one MSI-X entry (entry 0) with its own vector.
 *
 * Both virtqueues are pointed at entry 0 when they are created;
 * configuration-change interrupts are disabled.
 *
 * @return @c 0 on success, @c -1 if MSI-X could not be set up.
 */
static int setup_msix(struct virtio_net_dev *dev)
{
    struct pci_device *pci = dev->pci;

    if (pci_msix_alloc(pci, 1, virtio_net_irq_handler, dev) < 0)
        return -1;

    if (virtio_set_config_msix(dev->vdev, VIRTIO_MSI_NO_VECTOR) < 0)
        kprintf("[VIRTIO-NET] WARNING: config MSI-X vector not cleared\n");

    dev->irq_vector = pci->msix_vectors[0];
    kprintf("[VIRTIO-NET] Using MSI-X vector %u\n", dev->irq_vector);
    return 0;
}

/**
 * @brief Fall back to the device's INTx line via the I/O APIC.
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
    kprintf("[VIRTIO-NET] Using INTx IRQ %u (vector %u)\n",
            line, dev->irq_vector);
    return 0;
}

// ── RX: zero-copy burst ────────────────────────────────────────────────

/**
 * @brief Post free pool buffers on the RX queue, with one doorbell.
 *
 * Each buffer is posted so that the device writes the virtio header into
 * the device headroom and the frame's first byte lands on @c nb->head.
 * If the pool is empty, the ring just stays short until a later call.
 *
 * @return Number of buffers posted.
 */
static uint16_t rx_refill(struct virtio_net_dev *dev)
{
    uint16_t posted = 0;

    while (dev->rx_vq->num_free > 0)
    {
        struct netbuf *nb = netbuf_pool_alloc(dev->rx_pool);
        if (!nb)
            break;

        uint32_t room = (uint32_t)(nb->end - nb->head);
        virtqueue_add_buf(dev->rx_vq, nb->phys_addr - VIRTIO_NET_HDR_SIZE,
                          VIRTIO_NET_HDR_SIZE + room, true, nb);
        posted++;
    }

    if (posted)
        virtqueue_kick(dev->rx_vq);
    return posted;
}

/// @c netdev_ops.rx_burst: hand out completed RX buffers, then refill.
static uint16_t virtio_netdev_rx_burst(struct netdev *ndev, uint16_t qid,
                                       struct netbuf **pkts, uint16_t max)
{
    (void)qid;  // single queue for now
    struct virtio_net_dev *dev = ndev->priv;
    uint16_t n = 0;

    while (n < max)
    {
        uint32_t len;
        struct netbuf *nb = virtqueue_get_buf(dev->rx_vq, &len);
        if (!nb)
            break;

        if (len <= VIRTIO_NET_HDR_SIZE
            || !netbuf_put_quiet(nb, len - VIRTIO_NET_HDR_SIZE))
        {
            dev->rx_dropped++;
            netbuf_free(nb);
            continue;
        }
        pkts[n++] = nb;
    }

    rx_refill(dev);
    return n;
}

/// @c netdev_ops.rx_irq: the RX queue's interrupt on or off.
static void virtio_netdev_rx_irq(struct netdev *ndev, uint16_t qid, bool enable)
{
    (void)qid;
    struct virtio_net_dev *dev = ndev->priv;

    if (enable)
        virtqueue_enable_interrupts(dev->rx_vq);
    else
        virtqueue_disable_interrupts(dev->rx_vq);
}

// ── TX: zero-copy burst ────────────────────────────────────────────────

/// Free every netbuf the device has finished sending.
static void tx_reclaim(struct virtio_net_dev *dev)
{
    struct netbuf *nb;
    while ((nb = virtqueue_get_buf(dev->tx_vq, NULL)) != NULL)
        netbuf_free(nb);
}

/**
 * @brief @c netdev_ops.tx_burst: post netbufs directly, one doorbell.
 *
 * The zeroed virtio header goes into the space below @c nb->data, which
 * the device headroom guarantees for every netbuf.
 */
static uint16_t virtio_netdev_tx_burst(struct netdev *ndev, uint16_t qid,
                                       struct netbuf **pkts, uint16_t n)
{
    (void)qid;  // single queue for now
    struct virtio_net_dev *dev = ndev->priv;

    tx_reclaim(dev);

    uint16_t sent = 0;
    while (sent < n && dev->tx_vq->num_free > 0)
    {
        struct netbuf *nb = pkts[sent];

        memset(nb->data - VIRTIO_NET_HDR_SIZE, 0, VIRTIO_NET_HDR_SIZE);
        virtqueue_add_buf(dev->tx_vq, netbuf_data_phys(nb) - VIRTIO_NET_HDR_SIZE,
                          VIRTIO_NET_HDR_SIZE + nb->len, false, nb);
        sent++;
    }

    if (sent)
        virtqueue_kick(dev->tx_vq);
    return sent;
}

// ── netdev glue ────────────────────────────────────────────────────────

static const struct netdev_ops virtio_netdev_ops =
{
    .rx_burst = virtio_netdev_rx_burst,
    .tx_burst = virtio_netdev_tx_burst,
    .rx_irq   = virtio_netdev_rx_irq,
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

    // Negotiate features (VERSION_1 is implied) and confirm FEATURES_OK
    if (virtio_negotiate_features(vdev,
                                  VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS) < 0)
        goto fail;

    // Read the MAC address (if the feature was negotiated), consistently
    // with respect to config_generation
    if ((vdev->features & VIRTIO_NET_F_MAC)
        && virtio_read_config(vdev, VIRTIO_NET_CFG_MAC,
                              dev->mac, ETH_ALEN) < 0)
        kprintf("[VIRTIO-NET] WARNING: MAC outside device config\n");

    kprintf("[VIRTIO-NET] MAC: %s\n", ETH_FMT(dev->mac));

    // Claim the slot before interrupts can fire
    instances[instance_count] = dev;

    // Interrupt: MSI-X entry 0 with an allocated vector, else INTx. Set
    // up before the queues, which are created pointing at the entry
    uint16_t entry = VIRTIO_MSI_NO_VECTOR;
    if (pci->cap_msix && setup_msix(dev) == 0)
        entry = 0;
    else if (setup_intx(dev) < 0)
        kprintf("[VIRTIO_NET] WARNING: No interrupt configured "
                "(polling only)\n");

    // Create RX and TX virtqueues (registered in vdev->queues[])
    dev->rx_vq = virtqueue_create(vdev, VIRTIO_NET_RXQ, entry);
    dev->tx_vq = dev->rx_vq ? virtqueue_create(vdev, VIRTIO_NET_TXQ, entry)
                            : NULL;
    if (!dev->tx_vq)
    {
        kprintf("[VIRTIO-NET] Failed to create virtqueues\n");
        goto fail;
    }

    // Polled by default
    virtqueue_disable_interrupts(dev->rx_vq);
    virtqueue_disable_interrupts(dev->tx_vq);

    // The device can start receiving as soon as it goes live
    dev->rx_pool = netbuf_pool_create(smp_core_id());
    if (!dev->rx_pool)
        goto fail;
    uint16_t posted = rx_refill(dev);
    kprintf("[VIRTIO-NET] Pool of %u buffers, %u posted\n",
            dev->rx_pool->capacity, posted);

    // Mark the device as live
    virtio_set_status(vdev, VIRTIO_STATUS_DRIVER_OK);

    // Verify the device accepted the configuration
    uint8_t status = virtio_get_status(vdev);
    if (status & (VIRTIO_STATUS_FAILED | VIRTIO_STATUS_NEEDS_RESET))
    {
        kprintf("[VIRTIO-NET] ERROR: Device rejected configuration "
                "(status=0x%x)\n", status);
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
    netbuf_pool_destroy(dev->rx_pool);
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
    { PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET },         // transitional
    { PCI_VENDOR_VIRTIO, VIRTIO_DEV_NET_MODERN },  // modern-only
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
    uint16_t status = virtio_read_config16(dev->vdev, VIRTIO_NET_CFG_STATUS);
    return (status & VIRTIO_NET_S_LINK_UP) != 0;
}

