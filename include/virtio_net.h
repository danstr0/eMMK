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
 * @par RX model
 * The driver pre-populates the RX queue with empty buffers. When the
 * device receives a packet, it fills a buffer and places it on the
 * used ring. The driver processes completions via polling
 * (@c virtio_net_poll) or interrupt (@c virtio_net_irq_handler).
 *
 * For a run-to-completion architecture, register a receive callback
 * with @c virtio_net_set_rx_callback() to process packets inline.
 */

#ifndef EMMK_VIRTIO_NET_H
#define EMMK_VIRTIO_NET_H

#include "types.h"
#include "virtio.h"
#include "idt.h"

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

/// Header flags.
#define VIRTIO_NET_HDR_F_NEEDS_CSUM  1  ///< Checksum needs computation.
#define VIRTIO_NET_HDR_F_DATA_VALID  2  ///< Checksum is valid (RX).

/// GSO types.
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
/// @}

/// @name Virtqueue indices
/// @{
#define VIRTIO_NET_RXQ  0  ///< Receive queue.
#define VIRTIO_NET_TXQ  1  ///< Transmit queue.
#define VIRTIO_NET_CTQ  2  ///< Control queue (optional, unused).
/// @}

/// @name Interrupt vector
/// @{

/// IDT vector used for virtio-net MSI-X interrupts.
#define VIRTIO_NET_MSIX_VECTOR  50
/// @}

/// @name MSI-X vector registers
/// @{
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR   20  ///< 16-bit: MSI-X vector for selected queue.
#define VIRTIO_PCI_CONFIG_MSIX_VECTOR  22  ///< 16-bit: MSI-X vector for config changes.
#define VIRTIO_MSI_NO_VECTOR           0xFFFF  ///< Disable MSI-X for this queue/config.
/// @}

/// @name Receive callback
/// @{

/**
 * @brief Per-packet receive callback type.
 *
 * Invoked inline during @c virtio_net_poll() or
 * @c virtio_net_irq_handler() for each completed RX packet.
 *
 * @param data  Pointer to Ethernet frame (after the virtio-net
 *              header has been stripped). Valid only for the duration
 *              of the callback — copy if needed.
 * @param len   Frame length in bytes (excluding the virtio-net header).
 * @param ctx   Opaque context set via @c virtio_net_set_rx_callback().
 *
 * @note The callback must @b not free @p data - the driver manages
 *       the buffer pool.
 */
typedef void (*virtio_net_rx_callback_t)(const void *data, size_t len,
                                         void *ctx);
/// @}

/// @name Device structure
/// @{

/// State for a single virtio-net device.
struct virtio_net_dev
{
    struct virtio_device *vdev;  ///< Underlying virtio transport.

    uint8_t mac[6];              ///< MAC address.

    struct virtqueue *rx_vq;     ///< Receive virtqueue.
    struct virtqueue *tx_vq;     ///< Transmit virtqueue.

    /// Pre-allocated RX buffer pool.
    struct
    {
        void     *virt;          ///< Virtual address.
        uint64_t  phys;          ///< Physical address.
    } rx_bufs[VIRTIO_NET_RX_BUFFERS];

    /// Receive callback (@c NULL = use @c virtio_net_receive instead).
    virtio_net_rx_callback_t rx_callback;
    void *rx_callback_ctx;

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
 * @brief Probe and initialize the first virtio-net device.
 *
 * Scans PCI for a virtio-net device, performs the virtio handshake
 * (reset → features → queues → DRIVER_OK), reads the MAC address,
 * allocates RX buffers, and pre-fills the RX queue.
 *
 * @return Pointer to the device, or @c NULL if not found.
 */
struct virtio_net_dev *virtio_net_init(void);
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
 * @note For high-throughput paths, prefer @c virtio_net_poll() with a
 *       registered callback — it batches completions and kicks.
 */
int virtio_net_receive(struct virtio_net_dev *dev, void *buf, size_t buflen);

/**
 * @brief Process all pending RX and TX completions.
 *
 * Drains the RX used ring, invoking the registered callback for each
 * packet, refills RX slots, and reclaims completed TX buffers. This
 * is the primary hot-path entry point for a polled operation.
 *
 * @param dev  Virtio-net device.
 */
void virtio_net_poll(struct virtio_net_dev *dev);

/**
 * @brief Register a per-packet receive callback.
 *
 * @param dev       Virtio-net device.
 * @param callback  Function to call per packet, or @c NULL to clear.
 * @param ctx       Opaque context passed to @p callback.
 */
void virtio_net_set_rx_callback(struct virtio_net_dev *dev,
                                virtio_net_rx_callback_t callback,
                                void *ctx);
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
 * @brief Top-level ISR for virtio-net interrupts.
 *
 * Reads the ISR status register to acknowledge the interrupt, then
 * calls @c virtio_net_poll() to process completions. Registered via
 * @c idt_set_handler() during @c virtio_net_init().
 *
 * @param frame  Interrupt frame (from the IDT dispatcher).
 */
void virtio_net_irq_handler(struct interrupt_frame *frame);
/// @}

#endif // EMMK_VIRTIO_NET_H

