/**
 * @file virtio_net.c
 * @brief Virtio network device driver.
 *
 * @par Probe sequence (modern transport), once per virtio-net device:
 * 1. @c virtio_device_init() — map the register regions, reset,
 *    ACKNOWLEDGE, DRIVER.
 * 2. Negotiate features (VERSION_1, MAC, STATUS, CTRL_VQ, MQ, RSS) with
 *    FEATURES_OK, and read the MAC.
 * 3. Decide the queue pairs: as many as the device, the cores, and
 *    @c VIRTIO_NET_MAX_PAIRS allow (one without a control queue).
 * 4. Interrupt: one MSI-X entry per pair, each with a vector from the
 *    kernel allocator and targeted at the pair's core (or the shared INTx
 *    line as a fallback). Before the queues: each queue's MSI-X entry is
 *    programmed when it is created.
 * 5. Create each pair's RX and TX virtqueues (interrupts suppressed), its
 *    netbuf pool (owner: core k), and fill its ring; then the control
 *    queue.
 * 6. Set DRIVER_OK — the device is live, delivering to pair 0 only.
 * 7. Register a @c netdev (@c eth0, @c eth1, ...) for it.
 * More pairs are switched on later with @c netdev_set_queues(), through
 * the control queue: device RSS where negotiated, else MQ steering.
 *
 * @par Transmit path
 * @c tx_burst writes a zeroed @c virtio_net_hdr into the headroom just
 * below the frame, posts the netbuf itself, and rings one doorbell.
 * Completed netbufs are freed by the next @c tx_burst on that pair.
 *
 * @par Receive path
 * Each pair's ring holds netbufs from that pair's pool; the device writes
 * header + frame straight into them. @c rx_burst hands those same netbufs
 * to the caller and refills the ring from the pool with one doorbell.
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
#include "timer.h"

// ── Instance table ─────────────────────────────────────────────────────

/// Every probed device, in probe order.
static struct virtio_net_dev *instances[VIRTIO_NET_MAX_DEVICES];
static int instance_count = 0;

/// Vector allocated for each INTx line (0 = none yet). Several devices
/// may share one line; its handler acknowledges all of them.
static uint8_t intx_vectors[16];

/// The well-known Toeplitz key (Microsoft RSS specification), so that
/// the device's spreading can be reproduced by any standard tool.
static const uint8_t rss_key[VIRTIO_NET_RSS_KEY_LEN] =
{
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
    0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
    0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
    0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
    0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
};

// ── Interrupt handlers ─────────────────────────────────────────────────

void virtio_net_irq_handler(struct interrupt_frame *frame)
{
    struct virtio_net_dev *dev = irq_vector_ctx(frame->vector);

    // With MSI-X the device does not use the ISR status register; there
    // is nothing to acknowledge at the device. Each pair's interrupt
    // goes to its own core.
    if (dev)
        __atomic_fetch_add(&dev->irq_count, 1, __ATOMIC_RELAXED);

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
            __atomic_fetch_add(&dev->irq_count, 1, __ATOMIC_RELAXED);
    }

    lapic_eoi();
}

/**
 * @brief One MSI-X entry per queue pair, each target at its core.
 *
 * Entry @e i (vector @c pci->msix_vectors[i]) interrupts core @e i. If
 * the table has fewer entries than there are pairs, pairs share them
 * (pair @e k uses entry `k mod entries`). Configuration-change interrupts
 * are disabled.
 *
 * @return @c 0 on success, @c -1 if MSI-X could not be set up.
 */
static int setup_msix(struct virtio_net_dev *dev)
{
    struct pci_device *pci = dev->pci;

    if (pci_msix_enable(pci) < 0)
        return -1;

    uint16_t n = dev->max_pairs;
    if (n > pci->msix_count)
        n = pci->msix_count;
    if (n > PCI_MSIX_MAX_VECTORS)
        n = PCI_MSIX_MAX_VECTORS;

    if (pci_msix_alloc(pci, n, virtio_net_irq_handler, dev) < 0)
        return -1;

    for (uint16_t i = 0; i < n; ++i)
        pci_msix_set_entry(pci, i, pci->msix_vectors[i],
                           smp_get_core(i)->apic_id);

    if (virtio_set_config_msix(dev->vdev, VIRTIO_MSI_NO_VECTOR) < 0)
        kprintf("[VIRTIO-NET] WARNING: config MSI-X vector not cleared\n");

    dev->msix_entries = n;
    dev->irq_vector   = pci->msix_vectors[0];
    kprintf("[VIRTIO-NET] Using %u MSI-X vector(s), first %u\n",
            n, dev->irq_vector);
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
 * @brief Post free pool buffers on a pair's RX queue, with one doorbell.
 *
 * Each buffer is posted so that the device writes the virtio header into
 * the device headroom and the frame's first byte lands on @c nb->head.
 * If the pool is empty, the ring just stays short until a later call.
 *
 * @return Number of buffers posted.
 */
static uint16_t rx_refill(struct virtio_net_queue *q)
{
    uint16_t posted = 0;

    while (q->rx_vq->num_free > 0)
    {
        struct netbuf *nb = netbuf_pool_alloc(q->rx_pool);
        if (!nb)
            break;

        uint32_t room = (uint32_t)(nb->end - nb->head);
        virtqueue_add_buf(q->rx_vq, nb->phys_addr - VIRTIO_NET_HDR_SIZE,
                          VIRTIO_NET_HDR_SIZE + room, true, nb);
        posted++;
    }

    if (posted)
        virtqueue_kick(q->rx_vq);
    return posted;
}

/// @c netdev_ops.rx_burst: hand out completed RX buffers, then refill.
static uint16_t virtio_netdev_rx_burst(struct netdev *ndev, uint16_t qid,
                                       struct netbuf **pkts, uint16_t max)
{
    struct virtio_net_dev *dev = ndev->priv;
    struct virtio_net_queue *q = &dev->q[qid];
    uint16_t n = 0;

    while (n < max)
    {
        uint32_t len;
        struct netbuf *nb = virtqueue_get_buf(q->rx_vq, &len);
        if (!nb)
            break;

        if (len <= VIRTIO_NET_HDR_SIZE
            || !netbuf_put_quiet(nb, len - VIRTIO_NET_HDR_SIZE))
        {
            q->rx_dropped++;
            netbuf_free(nb);
            continue;
        }
        pkts[n++] = nb;
    }

    rx_refill(q);
    return n;
}

/// @c netdev_ops.rx_irq: a pair's RX interrupt on or off.
static void virtio_netdev_rx_irq(struct netdev *ndev, uint16_t qid, bool enable)
{
    struct virtio_net_dev *dev = ndev->priv;

    if (enable)
        virtqueue_enable_interrupts(dev->q[qid].rx_vq);
    else
        virtqueue_disable_interrupts(dev->q[qid].rx_vq);
}

// ── TX: zero-copy burst ────────────────────────────────────────────────

/// Free every netbuf the device has finished sending on @p q.
static void tx_reclaim(struct virtio_net_queue *q)
{
    struct netbuf *nb;
    while ((nb = virtqueue_get_buf(q->tx_vq, NULL)) != NULL)
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
    struct virtio_net_dev *dev = ndev->priv;
    struct virtio_net_queue *q = &dev->q[qid];

    tx_reclaim(q);

    uint16_t sent = 0;
    while (sent < n && q->tx_vq->num_free > 0)
    {
        struct netbuf *nb = pkts[sent];

        memset(nb->data - VIRTIO_NET_HDR_SIZE, 0, VIRTIO_NET_HDR_SIZE);
        virtqueue_add_buf(q->tx_vq, netbuf_data_phys(nb) - VIRTIO_NET_HDR_SIZE,
                          VIRTIO_NET_HDR_SIZE + nb->len, false, nb);
        sent++;
    }

    if (sent)
        virtqueue_kick(q->tx_vq);
    return sent;
}

// ── Control queue ──────────────────────────────────────────────────────

// Layout of the command page: class/command, then data, then the ack
#define CTRL_HDR_OFF   0
#define CTRL_DATA_OFF  64
#define CTRL_ACK_OFF   (PAGE_SIZE - 64)

/**
 * @brief Run one control command and wait for the device's answer.
 *
 * Synchronous (control plane isn't latency-sensitive): posts the three-part
 * chain (header, data, ack), rings, and spins up to 100 ms.
 * Control commands must come from one core at a time.
 *
 * @param data  Command data, already in the page at @c CTRL_DATA_OFF.
 * @return @c 0 if the device answered @c VIRTIO_NET_OK.
 */
static int ctrl_command(struct virtio_net_dev *dev, uint8_t cls,
                        uint8_t cmd, size_t data_len)
{
    if (!dev->ctrl_vq || !dev->ctrl_buf
        || data_len > CTRL_ACK_OFF - CTRL_DATA_OFF)
        return -1;

    uint8_t *page = dev->ctrl_buf;
    page[CTRL_HDR_OFF]     = cls;
    page[CTRL_HDR_OFF + 1] = cmd;
    page[CTRL_ACK_OFF]     = 0xFF;  // the device overwrites it

    struct virtio_buf bufs[3] =
    {
        { dev->ctrl_phys + CTRL_HDR_OFF,  2,                  false },
        { dev->ctrl_phys + CTRL_DATA_OFF, (uint32_t)data_len, false },
        { dev->ctrl_phys + CTRL_ACK_OFF,  1,                  true },
    };
    if (virtqueue_add_chain(dev->ctrl_vq, bufs, 3, dev) < 0)
        return -1;
    virtqueue_kick(dev->ctrl_vq);

    uint64_t deadline = timer_deadline_ms(100);
    while (!virtqueue_get_buf(dev->ctrl_vq, NULL))
    {
        if (timer_deadline_expired(deadline))
        {
            kprintf("[VIRTIO-NET] Control command %u/%u timed out\n", cls, cmd);
            return -1;
        }
        cpu_relax();
    }

    return ((volatile uint8_t *)page)[CTRL_ACK_OFF] == VIRTIO_NET_OK ? 0 : -1;
}

/// Little-endian 16-bit store into the command page.
static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/**
 * @brief Program device RSS to spread flows over @p n pairs.
 *
 * Toeplitz hash over the IPv4 addresses (and UDP/TCP ports where
 * supported). Indirection entry @e i → pair `i mod n`. Frames that cannot
 * be hashed (ARP, ...) go to pair 0, whose core owns ARP.
 */
static int rss_configure(struct virtio_net_dev *dev, uint16_t n)
{
    uint8_t *d = dev->ctrl_buf + CTRL_DATA_OFF;
    uint16_t len = dev->rss_table_len;

    uint32_t types = dev->rss_hash_types;
    d[0] = (uint8_t)types;         d[1] = (uint8_t)(types >> 8);
    d[1] = (uint8_t)(types >> 16); d[3] = (uint8_t)(types >> 24);
    put16(d + 4, (uint16_t)(len - 1));  // indirection_table_mask
    put16(d + 6, 0);                    // unclassified_queue

    size_t off = 8;
    for (uint16_t i = 0; i < len; ++i, off += 2)
        put16(d + off, (uint16_t)(i % n));

    put16(d + off, n);  // max_tx_vq
    off += 2;
    d[off++] = dev->rss_key_len;
    memcpy(d + off, rss_key, dev->rss_key_len);
    off += dev->rss_key_len;

    return ctrl_command(dev, VIRTIO_NET_CTRL_MQ, VIRTIO_NET_CTRL_MQ_RSS_CONFIG, off);
}

/// @c netdev_ops.set_queues: deliver to @p n pairs.
static uint16_t virtio_netdev_set_queues(struct netdev *ndev, uint16_t n)
{
    struct virtio_net_dev *dev = ndev->priv;

    if (n > dev->max_pairs)
        n = dev->max_pairs;
    if (n == dev->active_pairs || !dev->ctrl_buf)
        return dev->active_pairs;

    int ret;
    if (dev->rss)
        ret = rss_configure(dev, n);
    else
    {
        put16(dev->ctrl_buf + CTRL_DATA_OFF, n);
        ret = ctrl_command(dev, VIRTIO_NET_CTRL_MQ,
                           VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET, 2);
    }

    if (ret < 0)
    {
        kprintf("[VIRTIO-NET] %s: device refused %u queue pair(s)\n",
                ndev->name, n);
        return dev->active_pairs;
    }

    dev->active_pairs = n;
    kprintf("[VIRTIO-NET] %s: %u queue pair(s), %s\n", ndev->name, n,
            dev->rss ? "device RSS" : "device steering (no RSS)");
    return n;
}

// ── netdev glue ────────────────────────────────────────────────────────

static const struct netdev_ops virtio_netdev_ops =
{
    .rx_burst   = virtio_netdev_rx_burst,
    .tx_burst   = virtio_netdev_tx_burst,
    .rx_irq     = virtio_netdev_rx_irq,
    .set_queues = virtio_netdev_set_queues,
};

/// Create, register, and bring up the netdev for @p dev.
static int register_netdev(struct virtio_net_dev *dev)
{
    // NULL name: netdev_register() picks the lowest free ethN
    struct netdev *ndev = netdev_alloc(NULL);
    if (!ndev)
        return -1;

    memcpy(ndev->mac, dev->mac, ETH_ALEN);
    ndev->ops        = &virtio_netdev_ops;
    ndev->priv       = dev;
    ndev->max_queues = dev->max_pairs;
    ndev->num_queues = dev->active_pairs;

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
 * @brief Decide the queue pairs and read the RSS limits.
 *
 * Sets @c max_pairs, @c rss and the RSS parameters, and returns the
 * control queue's index, or @c -1 if there is none (then one pair).
 */
static int plan_queues(struct virtio_net_dev *dev)
{
    struct virtio_device *vdev = dev->vdev;
    bool multi = (vdev->features & (VIRTIO_NET_F_MQ | VIRTIO_NET_F_RSS)) != 0;

    uint16_t dev_pairs = multi
        ? virtio_read_config16(vdev, VIRTIO_NET_CFG_MAX_VQ_PAIRS) : 1;
    if (dev_pairs == 0)
        dev_pairs = 1;

    int ctrl_idx = 2 * dev_pairs;
    if (!(vdev->features & VIRTIO_NET_F_CTRL_VQ)
        || ctrl_idx >= VIRTIO_MAX_QUEUES || ctrl_idx >= vdev->max_queues)
    {
        dev->max_pairs = 1;
        return -1;
    }

    uint16_t pairs = dev_pairs;
    if (pairs > VIRTIO_NET_MAX_PAIRS)
        pairs = VIRTIO_NET_MAX_PAIRS;
    if (pairs > smp_get_core_count())
        pairs = (uint16_t)smp_get_core_count();
    dev->max_pairs = pairs;

    if (vdev->features & VIRTIO_NET_F_RSS)
    {
        uint8_t  key_max   = virtio_read_config8(vdev, VIRTIO_NET_CFG_RSS_MAX_KEY);
        uint16_t table_max = virtio_read_config16(vdev, VIRTIO_NET_CFG_RSS_MAX_TABLE);
        uint32_t supported = virtio_read_config32(vdev, VIRTIO_NET_CFG_RSS_HASH_TYPES);

        uint16_t len = VIRTIO_NET_RSS_TABLE_LEN;
        while (len > table_max && len > 1)
            len /= 2;

        dev->rss_table_len = len;
        dev->rss_key_len   = key_max < VIRTIO_NET_RSS_KEY_LEN
                           ? key_max : VIRTIO_NET_RSS_KEY_LEN;
        dev->rss_hash_types = supported & (VIRTIO_NET_RSS_HASH_TYPE_IPv4
                                         | VIRTIO_NET_RSS_HASH_TYPE_TCPv4
                                         | VIRTIO_NET_RSS_HASH_TYPE_UDPv4);
        dev->rss = dev->rss_hash_types != 0 && dev->rss_key_len > 0;
    }

    kprintf("[VIRTIO-NET] Device offers %u queue pair(s); using up to %u%s\n",
            dev_pairs, pairs, dev->rss ? ", RSS" : "");
    return ctrl_idx;
}

/// Undo a partial probe (device already reset).
static void release_queues(struct virtio_net_dev *dev)
{
    for (int k = 0; k < VIRTIO_NET_MAX_PAIRS; ++k)
    {
        struct virtio_net_queue *q = &dev->q[k];
        if (q->tx_vq)
            virtqueue_destroy(q->tx_vq);
        if (q->rx_vq)
            virtqueue_destroy(q->rx_vq);
        netbuf_pool_destroy(q->rx_pool);
    }
    if (dev->ctrl_vq)
        virtqueue_destroy(dev->ctrl_vq);
    if (dev->ctrl_buf)
        pmm_free_page(dev->ctrl_phys);
}

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

    struct virtio_net_dev *dev = kaligned_alloc(sizeof(struct virtio_net_dev), 64);
    if (!dev)
    {
        kfree(vdev);
        return -1;
    }
    memset(dev, 0, sizeof(*dev));
    dev->vdev         = vdev;
    dev->pci          = pci;
    dev->intx_line    = -1;
    dev->active_pairs = 1;

    // Negotiate features (VERSION_1 is implied) and confirm FEATURES_OK
    uint64_t want = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_NET_F_CTRL_VQ
                  | VIRTIO_NET_F_MQ | VIRTIO_NET_F_RSS;
    if (virtio_negotiate_features(vdev, want) < 0)
        goto fail;

    // Read the MAC address (if the feature was negotiated), consistently
    // with respect to config_generation
    if ((vdev->features & VIRTIO_NET_F_MAC)
        && virtio_read_config(vdev, VIRTIO_NET_CFG_MAC,
                              dev->mac, ETH_ALEN) < 0)
        kprintf("[VIRTIO-NET] WARNING: MAC outside device config\n");

    kprintf("[VIRTIO-NET] MAC: %s\n", ETH_FMT(dev->mac));

    int ctrl_idx = plan_queues(dev);

    // Claim the slot before interrupts can fire
    instances[instance_count] = dev;

    // Interrupts: MSI-X entries per pair, else INTx. Set up before the
    // queues, which are created pointing at their entries.
    bool msix = pci->cap_msix && setup_msix(dev) == 0;
    if (!msix && setup_intx(dev) < 0)
        kprintf("[VIRTIO_NET] WARNING: No interrupt configured "
                "(polling only)\n");

    for (uint16_t k = 0; k < dev->max_pairs; ++k)
    {
        struct virtio_net_queue *q = &dev->q[k];
        uint16_t entry = msix ? (uint16_t)(k % dev->msix_entries)
                              : VIRTIO_MSI_NO_VECTOR;

        // TX completions never need to wake anyone: no vector at all
        q->rx_vq = virtqueue_create(vdev, 2 * k, entry);
        q->tx_vq = q->rx_vq ? virtqueue_create(vdev, 2 * k + 1,
                                               VIRTIO_MSI_NO_VECTOR)
                            : NULL;
        if (!q->tx_vq)
        {
            kprintf("[VIRTIO-NET] Failed to create queue pair %u\n", k);
            goto fail;
        }

        // Polled by default
        virtqueue_disable_interrupts(q->rx_vq);
        virtqueue_disable_interrupts(q->tx_vq);

        // Pair k's buffers belong to core k, which will poll it
        q->rx_pool = netbuf_pool_create(k);
        if (!q->rx_pool)
            goto fail;
        rx_refill(q);
    }

    if (ctrl_idx >= 0)
    {
        dev->ctrl_vq = virtqueue_create(vdev, (uint16_t)ctrl_idx,
                                        VIRTIO_MSI_NO_VECTOR);
        uint64_t page = dev->ctrl_vq ? pmm_alloc_page() : PMM_ALLOC_FAILED;
        if (page == PMM_ALLOC_FAILED)
        {
            // Still usable, with a single pair
            kprintf("[VIRTIO-NET] No control queue: single queue pair\n");
            dev->max_pairs = 1;
        }
        else
        {
            dev->ctrl_phys = page;
            dev->ctrl_buf  = PHYS_TO_VIRT(page);
            virtqueue_disable_interrupts(dev->ctrl_vq);
        }
    }

    kprintf("[VIRTIO-NET] %u pair(s), pools of %u buffers\n",
            dev->max_pairs, dev->q[0].rx_pool->capacity);

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
    release_queues(dev);
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

