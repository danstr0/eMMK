/**
 * @file virtio.h
 * @brief Virtio common definitions and virtqueue interface.
 *
 * Virtio is a standardized interface for paravirtual devices.
 * Communication between driver and device uses shared-memory ring
 * buffers (virtqueues):
 *
 * @par Driver → Device
 * 1. Driver fills descriptor(s) with buffer addresses.
 * 2. Driver adds descriptor head to the available ring.
 * 3. Driver "kicks" the device (writes to notification register).
 *
 * @par Device → Driver
 * 1. Device processes the buffers.
 * 2. Device adds the descriptor head to the used ring.
 * 3. Device raises an interrupt (if enabled).
 *
 * @par Virtqueue memory layout (contiguous, page-aligned)
 * @code
 *   [descriptors][available ring + padding to 4096][used ring]
 * @endcode
 *
 * This implementation uses the legacy (transitional) virtio PCI
 * interface, where configuration is accessed via I/O ports. The
 * modern (MMIO-based) interface is not implemented.
 *
 * @see Virtio Specification v1.3
 */

#ifndef EMMK_VIRTIO_H
#define EMMK_VIRTIO_H

#include "pci.h"
#include "types.h"
#include "kernel.h"

/// @name Virtio PCI capability types
/// @{ 
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5
/// @}

/// @name Legacy virtio PCI I/O port offsets
/// @{
#define VIRTIO_PCI_HOST_FEATURES   0  ///< 32-bit: device feature bits.
#define VIRTIO_PCI_GUEST_FEATURES  4  ///< 32-bit: driver feature bits.
#define VIRTIO_PCI_QUEUE_PFN       8  ///< 32-bit: queue PFN (phys >> 12).
#define VIRTIO_PCI_QUEUE_SIZE     12  ///< 16-bit: queue size (read-only).
#define VIRTIO_PCI_QUEUE_SEL      14  ///< 16-bit: queue selector.
#define VIRTIO_PCI_QUEUE_NOTIFY   16  ///< 16-bit: queue notification.
#define VIRTIO_PCI_DEVICE_STATUS  18  ///< 8-bit:  device status.
#define VIRTIO_PCI_ISR_STATUS     19  ///< 8-bit:  ISR status (read clears).

// Device-specific config starts at offset 20 for legacy devices.
// With MSI-X enabled, config moves to offset 24.
#define VIRTIO_PCI_CONFIG         20
#define VIRTIO_PCI_CONFIG_MSIX    24
/// @}

/// @name Device status bits
/// @{
#define VIRTIO_STATUS_RESET        0    ///< Write 0 to reset device.
#define VIRTIO_STATUS_ACKNOWLEDGE  1    ///< OS has found the device.
#define VIRTIO_STATUS_DRIVER       2    ///< OS knows how to drive this device.
#define VIRTIO_STATUS_DRIVER_OK    4    ///< Driver is ready.

// Feature negotiation complete (modern interface only).
#define VIRTIO_STATUS_FEATURES_OK  8

#define VIRTIO_STATUS_FAILED     128    ///< Something went wrong.
/// @}

/// @name Feature bits (common across device types)
/// @{
#define VIRTIO_F_RING_INDIRECT_DESC  (1ULL << 28)  ///< Indirect descriptors.
#define VIRTIO_F_RING_EVENT_IDX      (1ULL << 29)  ///< Event index suppression.

/**
 * Marks the modern (non-legacy) interface. Not negotiated — eMMK uses
 * the legacy transport.
 *
 * @note This constant is 64-bit (<code>1ULL << 32</code>), but the
 *       legacy feature negotiation functions use @c uint32_t. The bit is
 *       unreachable through the legacy interface by design.
 */
#define VIRTIO_F_VERSION_1           (1ULL << 32)
/// @}

/// @name Vring descriptor flags
/// @{
#define VRING_DESC_F_NEXT      1  ///< Descriptor continues via @c next.
#define VRING_DESC_F_WRITE     2  ///< Buffer is device-writable.
#define VRING_DESC_F_INDIRECT  4  ///< Contains a table of descriptors.
/// @}

/// @name Vring ring flags
/// @{ 
#define VRING_AVAIL_F_NO_INTERRUPT  1  ///< Suppress device interrupts.
#define VRING_USED_F_NO_NOTIFY      1  ///< Suppress driver notifications.
/// @}

/// @name Vring structures
/// @{

/// Descriptor table entry — points to a data buffer.
struct vring_desc
{
    uint64_t addr;   ///< Physical address of the buffer.
    uint32_t len;    ///< Length of the buffer in bytes.
    uint16_t flags;  ///< @c VRING_DESC_F_*.
    uint16_t next;   ///< Index of next descriptor (if @c VRING_DESC_F_NEXT).
} PACKED;

/// Available ring — driver writes, device reads.
struct vring_avail
{
    uint16_t flags;   ///< @c VRING_AVAIL_F_*.
    uint16_t idx;     ///< Next index the driver will write to.
    uint16_t ring[];  ///< Array of descriptor head indices.
    // Followed by uint16_t used_event (if EVENT_IDX negotiated)
} PACKED;

/// Used ring element — one completed descriptor chain.
struct vring_used_elem
{
    uint32_t id;   ///< Descriptor chain head index.
    uint32_t len;  ///< Total bytes written by the device.
} PACKED;

/// Used ring — device writes, driver reads.
struct vring_used
{
    uint16_t flags;     ///< @c VRING_USED_F_*.
    uint16_t idx;       ///< Next index the device will write to.
    struct vring_used_elem ring[];
    // Followed by uint16_t avail_event (if EVENT_IDX negotiated)
} PACKED;
/// @}

/// @name Buffer descriptor (for multi-buffer chains)
/// @{

/// Describes one buffer in a scatter-gather chain.
struct virtio_buf
{
    uint64_t addr;   ///< Physical address.
    uint32_t len;    ///< Length in bytes.
    bool     write;  ///< If @c true, device writes to this buffer.
};
/// @}

/// @name Virtio device
/// @{

/// Maximum number of virtqueues per device.
#define VIRTIO_MAX_QUEUES  8

/**
 * @brief Common state for any virtio PCI device.
 *
 * Created during device probe and passed to device-specific drivers
 * (e.g., virtio_net). Holds the PCI device, I/O base, negotiated
 * features, and all virtqueues.
 */
struct virtio_device
{
    struct pci_device *pci;  ///< Underlying PCI device.
    uint16_t io_base;        ///< Legacy I/O port base (BAR0).
    uint16_t config_offset;  ///< Config space offset (20 or 24).
    uint64_t features;       ///< Negotiated feature bits.
    uint8_t  num_queues;     ///< Number of active virtqueues.
    struct virtqueue *queues[VIRTIO_MAX_QUEUES];
};
/// @}

/// @name Virtqueue structure
/// @{

/**
 * @brief A single virtio ring buffer.
 *
 * Contains the three vring components (descriptors, available, used)
 * plus driver-side bookkeeping (free list, completion tracking).
 */
struct virtqueue
{
    uint16_t size;              ///< Number of descriptors (power of 2).
    uint16_t free_head;         ///< Head of the descriptor free list.
    uint16_t num_free;          ///< Number of free descriptors.
    uint16_t last_used_idx;     ///< Last used ring index we consumed.
    uint16_t queue_idx;         ///< Queue index (for notification writes).

    struct virtio_device *vdev; ///< Parent device.

    // The three rings, laid out contiguously in physical memory
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;

    /// Physical address of the virtqueue memory (for the device).
    uint64_t phys_addr;

    /// Per-descriptor opaque data (driver tracks in-flight requests).
    void **desc_data;
};
/// @}

/// @name Virtio device setup helpers
/// @{

/**
 * @brief Initialize a virtio device from its PCI device.
 *
 * Reads BAR0 for I/O base, performs the device reset and status
 * handshake (ACKNOWLEDGE + DRIVER), and reads device features.
 *
 * @param pci  PCI device (must be a virtio transitional device).
 * @return Pointer to the initialized @c virtio_device, or @c NULL.
 */
struct virtio_device *virtio_device_init(struct pci_device *pci);

/**
 * @brief Negotiate features with the device.
 *
 * ANDs @p desired with the device-offered features, writes the result
 * back, and stores the negotiated set in @c vdev->features.
 *
 * @param vdev     Virtio device.
 * @param desired  Feature bits the driver wants.
 * @return The negotiated feature bitmask (32-bit, legacy interface).
 *
 * @note The legacy interface exposes only 32 feature bits. 64-bit
 *       feature constants (e.g., @c VIRTIO_F_VERSION_1) are
 *       unreachable and silently dropped.
 */
uint32_t virtio_negotiate_features(struct virtio_device *vdev,
                                   uint32_t desired);

/**
 * @brief Set a device status bit.
 * @param vdev    Virtio device.
 * @param status  @c VIRTIO_STATUS_* bit(s) to OR into the current status.
 */
void virtio_set_status(struct virtio_device *vdev, uint8_t status);

/// Reset @p vdev (write 0 to the status register and wait for completion).
void virtio_reset(struct virtio_device *vdev);
/// @}

/// @name Device-specific config reads
/// @{
uint8_t  virtio_read_config8(struct virtio_device *vdev, uint8_t offset);
uint16_t virtio_read_config16(struct virtio_device *vdev, uint8_t offset);
uint32_t virtio_read_config32(struct virtio_device *vdev, uint8_t offset);
/// @}

/// @name Virtqueue API
/// @{

/**
 * @brief Allocate, initialize, and register a virtqueue.
 *
 * Selects the queue, reads its size from the device, allocates
 * page-aligned physically-contiguous vring memory, builds the
 * descriptor free list, writes the queue PFN to the device, and
 * registers the queue in @c vdev->queues[].
 *
 * @param vdev       Parent virtio device.
 * @param queue_idx  Queue index (0-based).
 * @return Pointer to the new virtqueue, or @c NULL on failure.
 */
struct virtqueue *virtqueue_create(struct virtio_device *vdev,
                                   uint16_t queue_idx);

/// Free @p vq and its backing memory.
void virtqueue_destroy(struct virtqueue *vq);

/**
 * @brief Add a single buffer to @p vq.
 *
 * @param vq         Virtqueue.
 * @param phys_addr  Physical address of the buffer.
 * @param len        Buffer length in bytes.
 * @param write      @c true if the device should write to this buffer.
 * @param data       Opaque pointer returned by @c virtqueue_get_buf()
 *                   on completion.
 * @return Descriptor index used, or @c -1 if no free descriptors.
 */
int virtqueue_add_buf(struct virtqueue *vq, uint64_t phys_addr,
                      uint32_t len, bool write, void *data);

/**
 * @brief Add a scatter-gather chain of buffers.
 *
 * @param vq     Virtqueue.
 * @param bufs   Array of buffer descriptors.
 * @param count  Number of buffers in the chain.
 * @param data   Opaque pointer for the whole chain.
 * @return Head descriptor index, or @c -1 on failure.
 */
int virtqueue_add_chain(struct virtqueue *vq, struct virtio_buf *bufs,
                        int count, void *data);

/// Notify the device that new buffers are available in @p vq.
void virtqueue_kick(struct virtqueue *vq);

/**
 * @brief Retrieve the next completed buffer from @p vq.
 *
 * @param[out] len  Bytes written by the device (may be @c NULL).
 * @return The @c data pointer passed to @c virtqueue_add_buf / 
 *         @c add_chain, or @c NULL if no completions are pending.
 */
void *virtqueue_get_buf(struct virtqueue *vq, uint32_t *len);

/// Check whether any completed buffers are available in @p vq.
bool virtqueue_has_buf(struct virtqueue *vq);

/// Suppress device interrupts for @p vq.
void virtqueue_disable_interrupts(struct virtqueue *vq);

/// Re-enable device interrupts for @p vq.
void virtqueue_enable_interrupts(struct virtqueue *vq);
/// @}

/// @name Memory size calculation
/// @{

/**
 * @brief Calculate total memory needed for a virtqueue.
 *
 * @param qsize  Number of descriptors (must be a power of 2).
 *
 * Layout per the virtio spec:
 * @code
 *   Descriptors:    16 * qsize bytes
 *   Available ring: 6 + 2 * qsize bytes (includes used_event)
 *   Padding to 4096
 *   Used ring:      6 + 8 * qsize bytes (includes avail_event)
 * @endcode
 */
static inline size_t virtqueue_mem_size(uint16_t qsize)
{
    size_t desc_size  = sizeof(struct vring_desc) * qsize;
    size_t avail_size = sizeof(uint16_t) * (3 + qsize);
    size_t first_part = PAGE_ALIGN_UP(desc_size + avail_size);
    size_t used_size  = sizeof(uint16_t) * 3
                        + sizeof(struct vring_used_elem) * qsize;
    return first_part + used_size;
}
/// @}

#endif // EMMK_VIRTIO_H

