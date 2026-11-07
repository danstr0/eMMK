/**
 * @file virtio.h
 * @brief Virtio common definitions, modern PCI transport, and virtqueues.
 *
 * Virtio is a standardized interface for paravirtual devices.
 * Communication between driver and device uses shared-memory ring
 * buffers (virtqueues):
 *
 * @par Driver → Device
 * 1. Driver fills descriptor(s) with buffer addresses.
 * 2. Driver adds descriptor head to the available ring.
 * 3. Driver "kicks" the device (writes to the queue's notify address).
 *
 * @par Device → Driver
 * 1. Device processes the buffers.
 * 2. Device adds the descriptor head to the used ring.
 * 3. Device raises an interrupt (if enabled).
 *
 * @par Transport
 * Only the virtio 1.x ("modern") PCI transport is implemented; the legacy
 * I/O port interface is not supported. The device's register regions are
 * found through vendor-specific PCI capabilities (`struct virtio_pci_cap`)
 * and mapped uncached with @c pci_map_bar():
 * - common configuration (features, status, queue setup),
 * - notification area (one doorbell per queue),
 * - ISR status (interrupt acknowledge for INTx),
 * - device-specific cnofiguration (e.g., the virtio-net MAC).
 * Features are 64-bit; @c VIRTIO_F_VERSION_1 is always negotiated and
 * the handshake includes @c FEATURES_OK.
 *
 * @par Virtqueue memory layout (contiguous, page-aligned)
 * @code
 *   [descriptors][available ring + padding to 4096][used ring]
 * @endcode
 * The modern transport takes the three parts as separate 64-bit
 * addresses; the contiguous layout is kept for simplicity.
 *
 * @see Virtio Specification v1.3
 */

#ifndef EMMK_VIRTIO_H
#define EMMK_VIRTIO_H

#include "pci.h"
#include "types.h"
#include "kernel.h"

/// @name Virtio PCI capability types (`virtio_pci_cap.cfg_type`)
/// @{
#define VIRTIO_PCI_CAP_COMMON_CFG  1  ///< Common configuration.
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2  ///< Notifications (doorbells).
#define VIRTIO_PCI_CAP_ISR_CFG     3  ///< ISR status.
#define VIRTIO_PCI_CAP_DEVICE_CFG  4  ///< Device-specific configuration.
#define VIRTIO_PCI_CAP_PCI_CFG     5  ///< Config-space access window (unused).
/// @}

/**
 * @brief Vendor-specific PCI capability locating one register region.
 *
 * Resides in PCI config space; read field by field with
 * @c pci_device_read*() at the offsets below.
 */
struct virtio_pci_cap
{
    uint8_t  cap_vndr;    ///< @c PCI_CAP_VENDOR (0x09).
    uint8_t  cap_next;    ///< Next capability.
    uint8_t  cap_len;     ///< Length of this capability.
    uint8_t  cfg_type;    ///< @c VIRTIO_PCI_CAP_*.
    uint8_t  bar;         ///< BAR holding the region.
    uint8_t  id;          ///< Distinguishes multiple caps of one type.
    uint8_t  padding[2];
    uint32_t offset;      ///< Region offset within the BAR.
    uint32_t length;      ///< Region length in bytes.
    // VIRTIO_PCI_CAP_NOTIFY_CFG: followed by uint32_t notify_off_multiplier
} PACKED;

/// @name @c virtio_pci_cap field offsets (in config space, from the cap)
/// @{
#define VIRTIO_PCI_CAP_CFG_TYPE      3
#define VIRTIO_PCI_CAP_BAR           4
#define VIRTIO_PCI_CAP_OFFSET        8
#define VIRTIO_PCI_CAP_LENGTH       12

/// notify_off_multiplier (notify cap only).
#define VIRTIO_PCI_NOTIFY_CAP_MULT  16 
/// @}

/// @name Common configuration register offsets
/// @{
#define VIRTIO_COMMON_DFSELECT       0x00  ///< 32: device_feature_select.
#define VIRTIO_COMMON_DF             0x04  ///< 32: device_feature (RO).
#define VIRTIO_COMMON_GFSELECT       0x08  ///< 32: driver_feature_select.
#define VIRTIO_COMMON_GF             0x0C  ///< 32: driver_feature.
#define VIRTIO_COMMON_MSIX           0x10  ///< 16: config_msix_vector.
#define VIRTIO_COMMON_NUMQ           0x12  ///< 16: num_queues (RO).
#define VIRTIO_COMMON_STATUS         0x14  ///<  8: device_status.
#define VIRTIO_COMMON_CFGGENERATION  0x15  ///<  8: config_generation (RO).
#define VIRTIO_COMMON_Q_SELECT       0x16  ///< 16: queue_select.
#define VIRTIO_COMMON_Q_SIZE         0x18  ///< 16: queue_size.
#define VIRTIO_COMMON_Q_MSIX         0x1A  ///< 16: queue_msix_vector.
#define VIRTIO_COMMON_Q_ENABLE       0x1C  ///< 16: queue_enable.
#define VIRTIO_COMMON_Q_NOFF         0x1E  ///< 16: queue_notify_off (RO).
#define VIRTIO_COMMON_Q_DESCLO       0x20  ///< 64: queue_desc, low
#define VIRTIO_COMMON_Q_DESCHI       0x24
#define VIRTIO_COMMON_Q_AVAILLO      0x28  ///< 64: available ring
#define VIRTIO_COMMON_Q_AVAILHI      0x2C
#define VIRTIO_COMMON_Q_USEDLO       0x30  ///< 64: used ring
#define VIRTIO_COMMON_Q_USEDHI       0x34
#define VIRTIO_COMMON_CFG_MIN_LEN    0x38  ///< Size of the fields above.

/// MSI-X entry value meaning "no interrupt" (queue or config).
#define VIRTIO_MSI_NO_VECTOR  0xFFFF
/// @}

/// @name Device status bits
/// @{
#define VIRTIO_STATUS_RESET         0  ///< Write 0 to reset device.
#define VIRTIO_STATUS_ACKNOWLEDGE   1  ///< OS has found the device.
#define VIRTIO_STATUS_DRIVER        2  ///< OS knows how to drive this device.
#define VIRTIO_STATUS_DRIVER_OK     4  ///< Driver is ready.
#define VIRTIO_STATUS_FEATURES_OK   8  ///< Feature negotiation complete.
#define VIRTIO_STATUS_NEEDS_RESET  64  ///< Device hit an error; needs a reset.
#define VIRTIO_STATUS_FAILED      128  ///< Something went wrong.
/// @}

/// @name Feature bits (common across device types)
/// @{
#define VIRTIO_F_RING_INDIRECT_DESC  (1UL << 28)  ///< Indirect descriptors.
#define VIRTIO_F_RING_EVENT_IDX      (1UL << 29)  ///< Event index suppression.
#define VIRTIO_F_VERSION_1           (1UL << 32)  ///< Modern device.
#define VIRTIO_F_ACCESS_PLATFORM     (1UL << 33)  ///< DMA goes through an IOMMU.
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
 * @brief Common state for any virtio PCI device (modern transport).
 *
 * Created by @c virtio_device_init() and passed to device-specific
 * drivers (e.g., virtio_net). Holds the PCI device, the mapped register
 * regions, negotiated features, and all virtqueues.
 */
struct virtio_device
{
    struct pci_device *pci;          ///< Underlying PCI device.
    
    volatile uint8_t *common;        ///< Common configuration region.
    volatile uint8_t *notify;        ///< Notification region.
    uint32_t notify_off_multiplier;  ///< Doorbell spacing.
    uint32_t notify_len;             ///< Notification region length.
    volatile uint8_t *isr;           ///< ISR status byte (read clears).
    volatile uint8_t *device_cfg;    ///< Device-specific configuration.
    uint32_t device_cfg_len;         ///< Device configuration length.

    uint64_t features;               ///< Negotiated feature bits.
    uint16_t max_queues;             ///< Queues the device implements.
    uint8_t  num_queues;             ///< Highest created queue index + 1.
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
    uint16_t queue_idx;         ///< Queue index (written to the doorbell).

    struct virtio_device *vdev; ///< Parent device.

    // The three rings, laid out contiguously in physical memory
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;

    /// Physical address of the virtqueue memory (for the device).
    uint64_t phys_addr;

    /// This queue's doorbell (in the notification region).
    volatile uint16_t *notify_addr;

    /// Per-descriptor opaque data (driver tracks in-flight requests).
    void **desc_data;
};
/// @}

/// @name Virtio device setup helpers
/// @{

/**
 * @brief Locate and map a modern virtio device's registers, then begin
 *        the handshake.
 *
 * Walks the vendor-specific capabilities, maps the common, notify, ISR,
 * and device regions (the first capability of each type, as the spec
 * recommends) through @c pci_map_bar(), enables the PCI device, resets
 * it, and sets ACKNOWLEDGE and DRIVER.
 *
 * @param pci  PCI device (transitional or modern-only virtio device).
 * @return The new @c virtio_device, or @c NULL if the device lacks the
 *         modern capabilities (legacy-only) or a region is invalid.
 */
struct virtio_device *virtio_device_init(struct pci_device *pci);

/**
 * @brief Negotiate features and complete the FEATURES_OK handshake.
 *
 * Reads the 64-bit device feature set, writes back
 * `offered & (desired | VIRTIO_F_VERSION_1)`, sets @c FEATURES_OK
 * and checks that the device kept it. The result is in @c vdev->features.
 *
 * @param vdev     Virtio device (after @c virtio_device_init()).
 * @param desired  Feature bits the driver wants (@c VIRTIO_F_VERSION_1).
 * @return @c 0 on success; @c -1 if the device does not offer
 *         @c VIRTIO_F_VERSION_1 or rejects the feature set (the status
 *         is then set to FAILED).
 */
int virtio_negotiate_features(struct virtio_device *vdev,
                              uint64_t desired);

/**
 * @brief Set a device status bit.
 * @param vdev    Virtio device.
 * @param status  @c VIRTIO_STATUS_* bit(s) to OR into the current status.
 */
void virtio_set_status(struct virtio_device *vdev, uint8_t status);

/// @return The device status register.
uint8_t virtio_get_status(struct virtio_device *vdev);

/**
 * @brief Reset @p vdev (write @c 0 to the status register and wait for it
 *        to read back @c 0).
 *
 * Also disables every queue: the modern transport has no per-queue
 * disable, so a reset is the only way to stop the device's DMA.
 */
void virtio_reset(struct virtio_device *vdev);

/**
 * @brief Set the MSI-X entry for configuration-change interrupts.
 * @param entry  Table entry, or @c VIRTIO_MSI_NO_VECTOR for none.
 * @return @c 0 if the device accepted it, @c -1 otherwise.
 */
int virtio_set_config_msix(struct virtio_device *vdev, uint16_t entry);

/**
 * @brief Read and clear the ISR status
 *
 * Needed only for INTx; with MSI-X the device does not use it.
 *
 * @return Bit 0: queue interrupt; bit 1: configuration change.
 */
uint8_t virtio_read_isr(struct virtio_device *vdev);
/// @}

/// @name Device-specific config reads
///
/// Single fields; reads past the region return @c 0. For a multi-field
/// value that must be consistent, use @c virtio_read_config().
/// @{
uint8_t  virtio_read_config8(struct virtio_device *vdev, uint32_t offset);
uint16_t virtio_read_config16(struct virtio_device *vdev, uint32_t offset);
uint32_t virtio_read_config32(struct virtio_device *vdev, uint32_t offset);

/**
 * @brief Copy @p len bytes of device configuration consistently.
 *
 * Retries while @c config_generation changes during the copy, so the
 * result never mixes values from before and after a device update.
 *
 * @return @c 0 on success, @c -1 if the range is outside the region.
 */
int virtio_read_config(struct virtio_device *vdev, uint32_t offset,
                       void *buf, size_t len);
/// @}

/// @name Virtqueue API
/// @{

/**
 * @brief Allocate, initialize, and enable a virtqueue.
 *
 * Selects the queue, reads its size, allocates page-aligned physically
 * contiguous vring memory, builds the descriptor free list, programs
 * the three ring addresses and the MSI-X entry, enables the queue, and
 * registers it in @c vdev->queues[]. Must be called before DRIVER_OK.
 *
 * @param vdev        Parent virtio device.
 * @param queue_idx   Queue index (0-based).
 * @param msix_entry  MSI-X table entry for this queue's interrupts, or
 *                    @c VIRTIO_MSI_NO_VECTOR (no MSI-X, or no interrupt).
 * @return Pointer to the new virtqueue, or @c NULL on failure (no such
 *         queue, allocation failure, or the device rejected the MSI-X
 *         entry).
 */
struct virtqueue *virtqueue_create(struct virtio_device *vdev,
                                   uint16_t queue_idx,
                                   uint16_t msix_entry);

/**
 * @brief Free @p vq and its backing memory.
 *
 * @warning The device must have been reset first (@c virtio_reset());
 *          an enabled queue may still be accessed by the device.
 */
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

/// Notify the device (MMIO doorbell write) that @p vq has new buffers.
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

/**
 * @brief Re-enable device interrupts for @p vq.
 *
 * Includes a full fence. A buffer completed just before the device saw
 * the flag raises no interrupt, so check the used ring once more before
 * sleeping.
 */
void virtqueue_enable_interrupts(struct virtqueue *vq);
/// @}

/// @name Memory size calculation
/// @{

/**
 * @brief Calculate total memory needed for a virtqueue.
 *
 * @param qsize  Number of descriptors (must be a power of 2).
 *
 * Layout used by eMMK:
 * @code{.txt}
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

