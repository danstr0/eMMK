/**
 * @file virtio_net.h
 * @brief Virtio network device driver interface.
 *
 * Drives virtio-net devices (transitional or modern-only) through the
 * virtio 1.x PCI transport. Uses two virtqueues for data transfer:
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
 * The driver keeps the RX ring filled with netbufs from the queue's pool.
 * When the device receives a packet, it writes straight into one and
 * places it on the used ring. Completions are processed only by polling
 * (@c netdev_rx_burst()), which hands out those same netbufs; there is
 * no copy. The interrupt is suppressed unless a core is about to idle,
 * and then it merely wakes that core.
 *
 * @warning Not thread-safe: the bursts of one device must run on a single
 *          core (the TX owner; see @c ip_set_tx_owner()), which must also
 *          own the pool for RX.
 */

#ifndef EMMK_VIRTIO_NET_H
#define EMMK_VIRTIO_NET_H

#include "types.h"
#include "virtio.h"
#include "idt.h"
#include "netbuf.h"

struct netdev;

/// @name Virtio-net feature bits (64-bit feature space)
/// @{
#define VIRTIO_NET_F_CSUM        (1UL << 0)   ///< Host handles TX checksum.
#define VIRTIO_NET_F_GUEST_CSUM  (1UL << 1)   ///< Guest handles RX checksum.
#define VIRTIO_NET_F_MAC         (1UL << 5)   ///< Device has given MAC.
#define VIRTIO_NET_F_GSO         (1UL << 6)   ///< Deprecated GSO support.
#define VIRTIO_NET_F_GUEST_TSO4  (1UL << 7)   ///< Guest can receive TSOv4.
#define VIRTIO_NET_F_GUEST_TSO6  (1UL << 8)   ///< Guest can receive TSOv6.
#define VIRTIO_NET_F_GUEST_ECN   (1UL << 9)   ///< Guest can receive TSO+ECN.
#define VIRTIO_NET_F_GUEST_UFO   (1UL << 10)  ///< Guest can receive UFO.
#define VIRTIO_NET_F_HOST_TSO4   (1UL << 11)  ///< Host can receive TSOv4.
#define VIRTIO_NET_F_HOST_TSO6   (1UL << 12)  ///< Host can receive TSOv6.
#define VIRTIO_NET_F_HOST_ECN    (1UL << 13)  ///< Host can receive TSO+ECN.
#define VIRTIO_NET_F_HOST_UFO    (1UL << 14)  ///< Host can receive UFO.
#define VIRTIO_NET_F_MRG_RXBUF   (1UL << 15)  ///< Merge RX buffers.
#define VIRTIO_NET_F_STATUS      (1UL << 16)  ///< Config status field valid.
#define VIRTIO_NET_F_CTRL_VQ     (1UL << 17)  ///< Control virtqueue present.
#define VIRTIO_NET_F_CTRL_RX     (1UL << 18)  ///< Control RX mode.
#define VIRTIO_NET_F_CTRL_VLAN   (1UL << 19)  ///< Control VLAN filtering.
#define VIRTIO_NET_F_MQ          (1UL << 22)  ///< Multiqueue.
#define VIRTIO_NET_F_HASH_REPORT (1UL << 57)  ///< Device reports flow hash.
#define VIRTIO_NET_F_RSS         (1UL << 60)  ///< Device-side RSS
/// @}

/// @name Device configuration offsets
/// @{
#define VIRTIO_NET_CFG_MAC     0  ///< 6 bytes: MAC address.
#define VIRTIO_NET_CFG_STATUS  6  ///< 16: @c VIRTIO_NET_S_*.
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
    
    /// RX: buffers used by this packet (1 without MRG_RXBUF);
    /// TX: 0.
    uint16_t num_buffers;
} PACKED;

_Static_assert(sizeof(struct virtio_net_hdr) == 12,
               "virtio 1.x net header is 12 bytes");

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

_Static_assert(sizeof(struct virtio_net_hdr) <= NETBUF_DEV_HEADROOM,
               "the virtio-net header must fit in the device headroom");
_Static_assert(NETBUF_DATA_SIZE >= 1514,
               "a pool buffer must hold a maximum Ethernet frame");

/// Maximum number of virtio-net devices the driver binds.
#define VIRTIO_NET_MAX_DEVICES  16
/// @}

/// @name Virtqueue indices
/// @{
#define VIRTIO_NET_RXQ  0  ///< Receive queue.
#define VIRTIO_NET_TXQ  1  ///< Transmit queue.
#define VIRTIO_NET_CTQ  2  ///< Control queue (optional, unused).
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

    /// PCI INTx line in use, or @c -1 (MSI-X or no interrupt).
    int     intx_line;

    /// Interrupts acknowledged for this device
    volatile uint64_t irq_count;

    struct virtqueue *rx_vq;      ///< Receive virtqueue.
    struct virtqueue *tx_vq;      ///< Transmit virtqueue.

    struct netbuf_pool *rx_pool;  ///< The RX queue's buffers.

    /// Frames the device delivered that the driver had to drop. Packet
    /// and byte counters reside in the netdev.
    uint64_t rx_dropped;
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
 * → DRIVER_OK), read the MAC adress, fills the RX ring from a new pool,
 * sets up one MSI-X vector from the allocator (INTx as a fallback),
 * and registers and brings up a netdev. IPv4 configuration is left to
 * the caller (@c netdev_set_ip()).
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
 * (@c irq_vector_ctx(frame->vector)), counts the interrupt and sends EOI.
 * Its only purpose is to wake a core idling in @c hlt; packets are
 * processed by the poll loop. Installed by @c pci_msix_alloc() during
 * probe.
 *
 * @param frame  Interrupt frame (from the IDT dispatcher).
 */
void virtio_net_irq_handler(struct interrupt_frame *frame);
/// @}

#endif // EMMK_VIRTIO_NET_H

