/**
 * @file virtio_net.h
 * @brief Virtio network device driver interface.
 *
 * Drives a virtio-net device using the legacy PCI interface. Uses two
 * virtqueues for data transfer:
 * - Queue 0 (RX): device writes received packets (pre-filled with
 *   buffers by the driver).
 * - Queue 1 (TX): driver writes packets to send.
 * - Queue 2 (Control): optional, for MAC filtering, VLAN, etc.
 *   (not used by this driver).
 *
 * @par Packet format
 * Every packet on the virtqueues is prefixed with a @c virtio_net_hdr
 * structure containing checksum offload and GSO metadata. For simple
 * operation (no offload), the header is zeroed.
 *
 * @par Driver model
 * @c virtio_net_register() registers a PCI driver; @c pci_init() then
 * probes @e every virtio-net device. Each device gets its own state,
 * interrupt vector (from the kernel allocator), and @c netdev
 * (@c eth0, @c eth1, ..., in probe order).
 *
 * @par RX model
 * The driver pre-populates the RX queue with empty buffers. When the
 * device receives a packet, it fills a buffer and places it on the
 * used ring. The driver processes completions only by polling
 * (@c virtio_net_poll, reached through @c netdev_poll()), copying each
 * frame into a @c netbuf for @c netdev_receive(); the interrupt merely
 * wakes an idle core.
 *
 * @warning Not thread-safe: @c virtio_net_send() and @c virtio_net_poll()
 *          for one device must be called from a single core (the TX
 *          owner; see @c ip_set_tx_owner()).
 */

#ifndef EMMK_VIRTIO_NET_H
#define EMMK_VIRTIO_NET_H

#include "types.h"
#include "virtio.h"
#include "idt.h"

struct netdev;

/// @name Virtio-net feature bits
/// @{
#define VIRTIO_NET_F_CSUM        (1U << 0)   ///< Host handles TX checksum.
#define VIRTIO_NET_F_GUEST_CSUM  (1U << 1)   ///< Guest handles RX checksum.
#define VIRTIO_NET_F_MAC         (1U << 5)   ///< Device has given MAC.
#define VIRTIO_NET_F_GSO         (1U << 6)   ///< Deprecated GSO support.
#define VIRTIO_NET_F_GUEST_TSO4  (1U << 7)   ///< Guest can receive TSOv4.
#define VIRTIO_NET_F_GUEST_TSO6  (1U << 8)   ///< Guest can receive TSOv6.
#define VIRTIO_NET_F_GUEST_ECN   (1U << 9)   ///< Guest can receive TSO+ECN.
#define VIRTIO_NET_F_GUEST_UFO   (1U << 10)  ///< Guest can receive UFO.
#define VIRTIO_NET_F_HOST_TSO4   (1U << 11)  ///< Host can receive TSOv4.
#define VIRTIO_NET_F_HOST_TSO6   (1U << 12)  ///< Host can receive TSOv6.
#define VIRTIO_NET_F_HOST_ECN    (1U << 13)  ///< Host can receive TSO+ECN.
#define VIRTIO_NET_F_HOST_UFO    (1U << 14)  ///< Host can receive UFO.
#define VIRTIO_NET_F_MRG_RXBUF   (1U << 15)  ///< Merge RX buffers.
#define VIRTIO_NET_F_STATUS      (1U << 16)  ///< Config status field valid.
#define VIRTIO_NET_F_CTRL_VQ     (1U << 17)  ///< Control virtqueue present.
#define VIRTIO_NET_F_CTRL_RX     (1U << 18)  ///< Control RX mode.
#define VIRTIO_NET_F_CTRL_VLAN   (1U << 19)  ///< Control VLAN filtering.
/// @}

/// @name Virtio-net header
/// @{

/**
 * @brief Metadata prepended to every RX and TX packet.
 *
 * For basic operation (no checksum offload, no GSO), the driver zeroes
 * the entire header before sending.
 */
struct virtio_net_hdr
{
    uint8_t  flags;        ///< @c VIRTIO_NET_HDR_F_*.
    uint8_t  gso_type;     ///< @c VIRTIO_NET_HDR_GSO_*.
    uint16_t hdr_len;      ///< Ethernet + IP + TCP/UDP header length.
    uint16_t gso_size;     ///< Maximum segment size for GSO.
    uint16_t csum_start;   ///< Offset to begin checksumming.
    uint16_t csum_offset;  ///< Offset to place the checksum.
    // If VIRTIO_NET_F_MRG_RXBUF: uint16_t num_buffers;
} PACKED;

#define VIRTIO_NET_HDR_SIZE  sizeof(struct virtio_net_hdr)

// Header flags
#define VIRTIO_NET_HDR_F_NEEDS_CSUM  1  ///< Checksum needs computation.
#define VIRTIO_NET_HDR_F_DATA_VALID  2  ///< Checksum is valid (RX).

// GSO types
#define VIRTIO_NET_HDR_GSO_NONE    0
#define VIRTIO_NET_HDR_GSO_TCPV4   1
#define VIRTIO_NET_HDR_GSO_UDP     3
#define VIRTIO_NET_HDR_GSO_TCPV6   4
#define VIRTIO_NET_HDR_GSO_ECN     0x80  ///< OR'd with a GSO type.
/// @}

/// @name Device-specific config layout (for reference)
/// @{

/**
 * @brief Virtio-net device configuration structure.
 *
 * Accessible via @c virtio_read_config{8,16,32}() at offsets into this
 * layout. Defined here for documentation; the driver reads fields
 * individually by offset rather than mapping the whole struct.
 */
struct virtio_net_config
{
    uint8_t  mac[6];               ///< Device MAC address.
    uint16_t status;               ///< @c VIRTIO_NET_S_* flags.
    uint16_t max_virtqueue_pairs;  ///< For multiqueue (if negotiated).
} PACKED;

#define VIRTIO_NET_S_LINK_UP    1  ///< Link is up.
#define VIRTIO_NET_S_ANNOUNCE   2  ///< Gratuitous ARP needed.
/// @}

/// @name Configuration constants
/// @{

/**
 * RX buffer size (bytes).
 *
 * Must be large enough for @c virtio_net_hdr (10 bytes) + maximum
 * Ethernet frame (1514 bytes) + headroom.
 */
#define VIRTIO_NET_RX_BUF_SIZE  2048

/// Number of pre-allocated RX buffer slots.
#define VIRTIO_NET_RX_BUFFERS   64

/// Maximum number of virtio-net devices the driver binds.
#define VIRTIO_NET_MAX_DEVICES  16
/// @}

/// @name Virtqueue indices
/// @{
#define VIRTIO_NET_RXQ  0  ///< Receive queue.
#define VIRTIO_NET_TXQ  1  ///< Transmit queue.
#define VIRTIO_NET_CTQ  2  ///< Control queue (optional, unused).
/// @}

/// @name MSI-X vector registers
/// @{
#define VIRTIO_PCI_CONFIG_MSIX_VECTOR  20  ///< 16-bit: MSI-X entry for config changes.
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR   22  ///< 16-bit: MSI-X entry for the selected queue.
#define VIRTIO_MSI_NO_VECTOR           0xFFFF  ///< Disable MSI-X for this queue/config.
/// @}

/// @name Device structure
/// @{

/// State for a single virtio-net device.
struct virtio_net_dev
{
    struct virtio_device *vdev;    ///< Underlying virtio transport.
    struct pci_device    *pci;     ///< PCI function this instance drives.
    struct netdev        *netdev;  ///< Network device registered for it.
    int                   index;   ///< Probe order (0 = first device).

    uint8_t mac[6];                ///< MAC address.

    /// IDT vector that wakes the core for this device (0 = none).
    uint8_t irq_vector;

    /// Legacy INTx line in use, or @c -1 (MSI-X or no interrupt).
    int intx_line;

    /// Interrupts acknowledged for this device
    volatile uint64_t irq_count;

    struct virtqueue *rx_vq;     ///< Receive virtqueue.
    struct virtqueue *tx_vq;     ///< Transmit virtqueue.

    /// Pre-allocated RX buffer pool.
    struct
    {
        void     *virt;          ///< Virtual address.
        uint64_t  phys;          ///< Physical address.
    } rx_bufs[VIRTIO_NET_RX_BUFFERS];
    int rx_buf_count;            ///< Entries of @c rx_bufs allocated.

    /// Statistics.
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
};
/// @}

/// @name Initialization
/// @{

/**
 * @brief Register the virtio-net PCI driver.
 *
 * Call before @pci_init() (which then probes every virtio-net device),
 * and after @c netdev_init() (each probe registers a netdev). For each
 * device, probe performs the virtio handshake (reset → features → queues
 * → DRIVER_OK), read the MAC adress, pre-fills the RX queue, sets up one
 * MSI-X vector from the allocator (legacy INTx as a fallback), and
 * registers and brings up a netdev. IPv4 configuration is left to the
 * caller (@c netdev_set_ip()).
 *
 * @return As @c pci_register_driver(): devices bound by this call (0
 *         before @c pci_init()), or @c -1 on failure.
 */
int virtio_net_register(void);

/// @return Number of virtio-net devices successfully probed.
int virtio_net_count(void);

/**
 * @return The device probed @p index-th (0-based), or @c NULL.
 * @note Its netdev is @c ->netdev.
 */
struct virtio_net_dev *virtio_net_get(int index);
/// @}

/// @name Transmit
/// @{

/**
 * @brief Transmit a raw Ethernet frame.
 *
 * Prepends a zeroed @c virtio_net_hdr, copies the payload into a
 * freshly allocated page, submits it to the TX queue, and kicks the
 * device. The caller's buffer is copied; @p data may be reused or
 * freed immediately after this call returns.
 *
 * @param dev   Virtio-net device.
 * @param data  Ethernet frame (starting at the destination MAC).
 * @param len   Frame length in bytes.
 * @return @c 0 on success, @c -1 on failure (queue full, alloc failure).
 */
int virtio_net_send(struct virtio_net_dev *dev, const void *data, size_t len);
/// @}

/// @name Receive
/// @{

/**
 * @brief Receive a single packet (polling, copy-out).
 *
 * Checks the RX used ring for a completed buffer. If available,
 * copies the packet (without @c virtio_net_hdr) into @p buf and
 * refills the RX slot.
 *
 * @param dev     Virtio-net device.
 * @param buf     Destination buffer.
 * @param buflen  Size of @p buf in bytes.
 * @return Number of bytes copied (> 0), @c 0 if no packet available,
 *         @c -1 on error.
 *
 * @warning Bypasses the netdev: do not mix with @c virtio_net_poll() on
 *          a device whose netdev is being polled.
 */
int virtio_net_receive(struct virtio_net_dev *dev, void *buf, size_t buflen);

/**
 * @brief Process all pending RX and TX completions.
 *
 * Drains the RX used ring, copies each frame into a @c netbuf queued on
 * the device's netdev (@c netdev_receive()), refills RX slots (one kick),
 * and reclaims completed TX buffers. Normally reached through
 * @c netdev_poll().
 *
 * @param dev  Virtio-net device.
 */
void virtio_net_poll(struct virtio_net_dev *dev);
/// @}

/// @name Status and configuration
/// @{

/// Copy the device's MAC address into @p mac (6 bytes).
void virtio_net_get_mac(struct virtio_net_dev *dev, uint8_t *mac);

/**
 * @brief Check whether the network link is up.
 *
 * Reads the device status register (requires @c VIRTIO_NET_F_STATUS).
 * If the feature was not negotiated, assumes the link is up.
 *
 * @param dev  Virtio-net device.
 * @return @c true if the link is up.
 */
bool virtio_net_link_up(struct virtio_net_dev *dev);
/// @}

/// @name Interrupt handler
/// @{

/**
 * @brief ISR for a virtio-net MSI-X vector.
 *
 * Finds its device through the vector's context
 * (@c irq_vector_ctx(frame->vector)), reads the ISR status register and
 * sends EOI. Its only purpose is to wake a core idling in @c hlt; packets
 * are processed by the poll loop. Installed by @c pci_msix_alloc() during
 * probe.
 *
 * @param frame  Interrupt frame (from the IDT dispatcher).
 */
void virtio_net_irq_handler(struct interrupt_frame *frame);
/// @}

#endif // EMMK_VIRTIO_NET_H

