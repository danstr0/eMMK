/**
 * @file virtio.c
 * @brief Virtio modern PCI transport.
 *
 * Implements the virtio 1.x PCI interface:
 * - Capability discovery: locate and map the common, notify, ISR, and
 *   device-specific register regions.
 * - Device initialization: reset, status handshake, 64-bit feature
 *   negotiation with FEATURES_OK.
 * - Virtqueue setup: allocate rings, build free list, program the ring
 *   addresses and MSI-X entry through the common config, enable.
 * - Descriptor management: alloc/free from a linked free list.
 * - Ring operations: add buffers, kick (MMIO doorbell), get completions.
 *
 * @par Memory ordering
 * Virtio requires specific ordering between ring updates and index
 * writes. On x86, stores are naturally ordered (no StoreStore
 * reordering), so a compiler barrier (@c barrier()) suffices before
 * updating @c avail->idx. A compiler barrier also suffices before
 * reading @c used->idx because x86 loads are ordered with respect to
 * other loads. Register regions are mapped uncached, so MMIO accesses
 * are not reordered with each other.
 */

#include "io.h"
#include "heap.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "virtio.h"
#include "vmm.h"

// ── Memory ordering helpers ────────────────────────────────────────────

/**
 * Driver → device barrier. On x86, stores are naturally ordered; a
 * compiler barrier prevents the compiler from reordering across this
 * point.
 */
#define virtio_wmb()  barrier()

/**
 * Device → driver barrier. On x86, loads are ordered with respect to
 * other loads; a compiler barrier suffices.
 */
#define virtio_rmb()  barrier()

/**
 * Full fence for cases where both directions must be ordered (e.g.,
 * before checking the @c NO_NOTIFY flag which the device may update).
 */
#define virtio_mb()   __asm__ volatile("mfence" ::: "memory")

// ── Common configuration access ────────────────────────────────────────

static inline uint8_t common_read8(struct virtio_device *vdev,
                                   uint32_t off)
{
    return mmio_read8(vdev->common + off);
}

static inline uint16_t common_read16(struct virtio_device *vdev,
                                     uint32_t off)
{
    return mmio_read16(vdev->common + off);
}

static inline uint32_t common_read32(struct virtio_device *vdev,
                                     uint32_t off)
{
    return mmio_read32(vdev->common + off);
}

static inline void common_write8(struct virtio_device *vdev,
                                 uint32_t off, uint8_t value)
{
    mmio_write8(vdev->common + off, value);
}

static inline void common_write16(struct virtio_device *vdev,
                                  uint32_t off, uint16_t value)
{
    mmio_write16(vdev->common + off, value);
}

static inline void common_write32(struct virtio_device *vdev,
                                  uint32_t off, uint32_t value)
{
    mmio_write32(vdev->common + off, value);
}

/// Write a 64-bit common-config field as two 32-bit halves (low first),
/// which every device must accept.
static inline void common_write64(struct virtio_device *vdev,
                                  uint32_t off_lo, uint64_t value)
{
    common_write32(vdev, off_lo,     (uint32_t)value);
    common_write32(vdev, off_lo + 4, (uint32_t)(value >> 32));
}

// ── Capability discovery ───────────────────────────────────────────────

/**
 * @brief Map the region described by the virtio capability at @p cap.
 *
 * @param[out] len  Region length.
 * @return Virtual address of the region, or @c NULL if its BAR is not a
 *         mappable memory BAR or the region does not fit inside it.
 */
static volatile uint8_t *map_cap_region(struct pci_device *pci,
                                        uint8_t cap, uint32_t *len)
{
    uint8_t  bar    = pci_device_read8(pci, cap + VIRTIO_PCI_CAP_BAR);
    uint32_t offset = pci_device_read32(pci, cap + VIRTIO_PCI_CAP_OFFSET);
    uint32_t length = pci_device_read32(pci, cap + VIRTIO_PCI_CAP_LENGTH);

    if (bar > 5 || length == 0
        || (uint64_t)offset + length > pci_get_bar_size(pci, bar))
        return NULL;

    uint8_t *base = pci_map_bar(pci, bar);
    if (!base)
        return NULL;

    *len = length;
    return (volatile uint8_t *)(base + offset);
}

/**
 * @brief Find and map every register region of @p vdev.
 *
 * Uses the first capability of each type (the spec allows several and
 * recommends the first one the driver understands).
 *
 * @return @c 0 if the common, notify, and ISR regions were all found.
 */
static int map_regions(struct virtio_device *vdev)
{
    struct pci_device *pci = vdev->pci;

    for (uint8_t cap = pci_find_next_capability(pci, PCI_CAP_VENDOR, 0);
         cap; cap = pci_find_next_capability(pci, PCI_CAP_VENDOR, cap))
    {
        uint8_t  type = pci_device_read8(pci, cap + VIRTIO_PCI_CAP_CFG_TYPE);
        uint32_t len = 0;
    
        switch (type)
        {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                if (!vdev->common)
                {
                    vdev->common = map_cap_region(pci, cap, &len);
                    if (vdev->common && len < VIRTIO_COMMON_CFG_MIN_LEN)
                        vdev->common = NULL;
                }
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                if (!vdev->notify)
                {
                    vdev->notify = map_cap_region(pci, cap, &len);
                    vdev->notify_len = len;
                    vdev->notify_off_multiplier =
                        pci_device_read32(pci, cap
                                          + VIRTIO_PCI_NOTIFY_CAP_MULT);
                }
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                if (!vdev->isr)
                    vdev->isr = map_cap_region(pci, cap, &len);
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                if (!vdev->device_cfg)
                {
                    vdev->device_cfg = map_cap_region(pci, cap, &len);
                    vdev->device_cfg_len = len;
                }
                break;
            default:
                break;
        }
    }

    return (vdev->common && vdev->notify && vdev->isr) ? 0 : -1;
}

// ── Virtio device setup ────────────────────────────────────────────────

struct virtio_device *virtio_device_init(struct pci_device *pci)
{
    if (!pci)
        return NULL;

    struct virtio_device *vdev = kzalloc(sizeof(struct virtio_device));
    if (!vdev)
        return NULL;

    vdev->pci = pci;

    if (map_regions(vdev) < 0)
    {
        kprintf("[VIRTIO] Device %x:%x.%x: no modern (virtio 1.x) "
                "register regions; legacy-only devices are not supported\n",
                pci->bus, pci->device, pci->function);
        kfree(vdev);
        return NULL;
    }

    // Memory decode (the regions) and bus mastering (the rings)
    pci_enable_device(pci);

    // Reset the device, then begin the status handshake
    virtio_reset(vdev);
    virtio_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(vdev, VIRTIO_STATUS_DRIVER);

    vdev->max_queues = common_read16(vdev, VIRTIO_COMMON_NUMQ);

    kprintf("[VIRTIO] Initialized device %x:%x.%x (modern), %u queues, "
            "notify multiplier %u\n",
            pci->bus, pci->device, pci->function, vdev->max_queues,
            vdev->notify_off_multiplier);

    return vdev;
}

int virtio_negotiate_features(struct virtio_device *vdev, uint64_t desired)
{
    // Read the device-offered features, 32 bits at a time
    common_write32(vdev, VIRTIO_COMMON_DFSELECT, 0);
    uint64_t offered = common_read32(vdev, VIRTIO_COMMON_DF);
    common_write32(vdev, VIRTIO_COMMON_DFSELECT, 1);
    offered |= (uint64_t)common_read32(vdev, VIRTIO_COMMON_DF) << 32;

    if (!(offered & VIRTIO_F_VERSION_1))
    {
        kprintf("[VIRTIO] Device does not offer VIRTIO_F_VERSION_1\n");
        virtio_set_status(vdev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    // Keep only features both sides support
    uint64_t negotiated = offered & (desired | VIRTIO_F_VERSION_1);

    common_write32(vdev, VIRTIO_COMMON_GFSELECT, 0);
    common_write32(vdev, VIRTIO_COMMON_GF, (uint32_t)negotiated);
    common_write32(vdev, VIRTIO_COMMON_GFSELECT, 1);
    common_write32(vdev, VIRTIO_COMMON_GF, (uint32_t)(negotiated >> 32));

    // The device must confirm the set by keeping FEATURES_OK
    virtio_set_status(vdev, VIRTIO_STATUS_FEATURES_OK);
    if (!(virtio_get_status(vdev) & VIRTIO_STATUS_FEATURES_OK))
    {
        kprintf("[VIRTIO] Device rejected features 0x%lx\n", negotiated);
        virtio_set_status(vdev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    vdev->features = negotiated;

    kprintf("[VIRTIO] Features: offered=0x%lx, desired=0x%lx, "
            "negotiated=0x%lx\n", offered, desired, negotiated);

    return 0;
}

void virtio_set_status(struct virtio_device *vdev, uint8_t status)
{
    uint8_t current = common_read8(vdev, VIRTIO_COMMON_STATUS);
    common_write8(vdev, VIRTIO_COMMON_STATUS, current | status);
}

uint8_t virtio_get_status(struct virtio_device *vdev)
{
    return common_read8(vdev, VIRTIO_COMMON_STATUS);
}

void virtio_reset(struct virtio_device *vdev)
{
    common_write8(vdev, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);

    // The reset is complete once the status reads back 0
    while (common_read8(vdev, VIRTIO_COMMON_STATUS) != 0)
        cpu_relax();
}

int virtio_set_config_msix(struct virtio_device *vdev, uint16_t entry)
{
    common_write16(vdev, VIRTIO_COMMON_MSIX, entry);
    return common_read16(vdev, VIRTIO_COMMON_MSIX) == entry ? 0 : -1;
}

uint8_t virtio_read_isr(struct virtio_device *vdev)
{
    return mmio_read8(vdev->isr);
}

// ── Device-specific configuration ──────────────────────────────────────

/// @return @c true if [offset, offset + len) lies inside the device config.
static inline bool config_in_range(struct virtio_device *vdev,
                                   uint32_t offset, size_t len)
{
    return vdev->device_cfg
           && (uint64_t)offset + len <= vdev->device_cfg_len;
}

uint8_t virtio_read_config8(struct virtio_device *vdev, uint32_t offset)
{
    return config_in_range(vdev, offset, 1)
           ? mmio_read8(vdev->device_cfg + offset) : 0;
}

uint16_t virtio_read_config16(struct virtio_device *vdev, uint32_t offset)
{
    return config_in_range(vdev, offset, 2)
           ? mmio_read16(vdev->device_cfg + offset) : 0;
}

uint32_t virtio_read_config32(struct virtio_device *vdev, uint32_t offset)
{
    return config_in_range(vdev, offset, 4)
           ? mmio_read32(vdev->device_cfg + offset) : 0;
}

int virtio_read_config(struct virtio_device *vdev, uint32_t offset,
                       void *buf, size_t len)
{
    if (!config_in_range(vdev, offset, len))
        return -1;

    uint8_t *out = buf;
    uint8_t before, after;
    do
    {
        before = common_read8(vdev, VIRTIO_COMMON_CFGGENERATION);
        for (size_t i = 0; i < len; ++i)
            out[i] = mmio_read8(vdev->device_cfg + offset + i);
        after = common_read8(vdev, VIRTIO_COMMON_CFGGENERATION);
    } while (before != after);

    return 0;
}

// ── Virtqueue creation / destruction ───────────────────────────────────

struct virtqueue *virtqueue_create(struct virtio_device *vdev,
                                   uint16_t queue_idx, uint16_t msix_entry)
{
    if (queue_idx >= VIRTIO_MAX_QUEUES || queue_idx >= vdev->max_queues)
    {
        kprintf("[VIRTIO] Queue index %u out of range (device has %u, "
                "max %u)\n",
                queue_idx, vdev->max_queues, VIRTIO_MAX_QUEUES);
        return NULL;
    }

    // Select the queue and read its size
    common_write16(vdev, VIRTIO_COMMON_Q_SELECT, queue_idx);
    uint16_t size = common_read16(vdev, VIRTIO_COMMON_Q_SIZE);

    if (size == 0)
    {
        kprintf("[VIRTIO] Queue %u does not exist (size=0)\n", queue_idx);
        return NULL;
    }

    // Split rings need a power-of-2 size
    if (size & (size - 1))
    {
        kprintf("[VIRTIO] Queue %u: invalid size %u (not power of 2)\n",
                queue_idx, size);
        return NULL;
    }

    // This queue's doorbell must lie inside the notification region
    uint32_t notify_off = (uint32_t)common_read16(vdev, VIRTIO_COMMON_Q_NOFF)
                        * vdev->notify_off_multiplier;
    if ((uint64_t)notify_off + sizeof(uint16_t) > vdev->notify_len)
    {
        kprintf("[VIRTIO] Queue %u: doorbell outside notify region\n",
                queue_idx);
        return NULL;
    }

    struct virtqueue *vq = kzalloc(sizeof(struct virtqueue));
    if (!vq)
        return NULL;

    vq->size          = size;
    vq->queue_idx     = queue_idx;
    vq->vdev          = vdev;
    vq->free_head     = 0;
    vq->num_free      = size;
    vq->last_used_idx = 0;
    vq->notify_addr   = (volatile uint16_t *)(vdev->notify + notify_off);

    // Allocate per-descriptor opaque data array
    vq->desc_data = kzalloc(sizeof(void *) * size);
    if (!vq->desc_data)
    {
        kfree(vq);
        return NULL;
    }

    // Allocate physically contiguous, page-aligned memory for the rings
    size_t total_size = virtqueue_mem_size(size);
    size_t pages      = PAGE_ALIGN_UP(total_size) / PAGE_SIZE;
    uint64_t phys     = pmm_alloc_pages(pages);
    if (phys == PMM_ALLOC_FAILED)
    {
        kprintf("[VIRTIO] Queue %u: failed to allocate %lu pages\n",
                queue_idx, (uint64_t)pages);
        kfree(vq->desc_data);
        kfree(vq);
        return NULL;
    }

    vq->phys_addr = phys;

    // Map and zero the ring memory
    void *virt = PHYS_TO_VIRT(phys);
    memset(virt, 0, pages * PAGE_SIZE);

    // Set up ring pointers per the virtio memory layout
    size_t desc_size   = sizeof(struct vring_desc) * size;
    size_t avail_size  = sizeof(uint16_t) * (3 + size);
    size_t used_offset = PAGE_ALIGN_UP(desc_size + avail_size);

    vq->desc  = (struct vring_desc *)virt;
    vq->avail = (struct vring_avail *)((uint8_t *)virt + desc_size);
    vq->used  = (struct vring_used *)((uint8_t *)virt + used_offset);

    // Build the descriptor free list (each points to the next)
    for (uint16_t i = 0; i < size - 1; ++i)
        vq->desc[i].next = i + 1;
    vq->desc[size - 1].next = 0xFFFF;

    // Program the queue (still selected) and its interrupt.
    common_write64(vdev, VIRTIO_COMMON_Q_DESCLO,  phys);
    common_write64(vdev, VIRTIO_COMMON_Q_AVAILLO, phys + desc_size);
    common_write64(vdev, VIRTIO_COMMON_Q_USEDLO,  phys + used_offset);

    common_write16(vdev, VIRTIO_COMMON_Q_MSIX, msix_entry);
    if (common_read16(vdev, VIRTIO_COMMON_Q_MSIX) != msix_entry)
    {
        kprintf("[VIRTIO] Queue %u: device rejected MSI-X entry %u\n",
                queue_idx, msix_entry);
        pmm_free_pages(phys, pages);
        kfree(vq->desc_data);
        kfree(vq);
        return NULL;
    }

    common_write16(vdev, VIRTIO_COMMON_Q_ENABLE, 1);

    // Register the queue in the parent device
    vdev->queues[queue_idx] = vq;
    if (queue_idx >= vdev->num_queues)
        vdev->num_queues = queue_idx + 1;

    kprintf("[VIRTIO] Queue %u: size=%u, phys=0x%lx, notify+0x%x\n",
            queue_idx, size, phys, notify_off);

    return vq;
}

void virtqueue_destroy(struct virtqueue *vq)
{
    if (!vq)
        return;

    // There is no per-queue disable; the caller has reset the device
    if (vq->vdev && vq->queue_idx < VIRTIO_MAX_QUEUES)
        vq->vdev->queues[vq->queue_idx] = NULL;

    // Free the ring memory
    size_t total_size = virtqueue_mem_size(vq->size);
    size_t pages      = PAGE_ALIGN_UP(total_size) / PAGE_SIZE;
    pmm_free_pages(vq->phys_addr, pages);

    kfree(vq->desc_data);
    kfree(vq);
}

// ── Descriptor free list ───────────────────────────────────────────────

static inline int alloc_desc(struct virtqueue *vq)
{
    if (vq->num_free == 0)
        return -1;

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return (int)idx;
}

static inline void free_desc(struct virtqueue *vq, uint16_t idx)
{
    vq->desc[idx].addr  = 0;
    vq->desc[idx].len   = 0;
    vq->desc[idx].flags = 0;
    vq->desc[idx].next  = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

/**
 * @brief Free an entire descriptor chain starting at @p head.
 *
 * Follows the @c VRING_DESC_F_NEXT pointers and returns each descriptor
 * to the free list.
 */
static void free_desc_chain(struct virtqueue *vq, uint16_t head)
{
    uint16_t idx = head;
    while (true)
    {
        uint16_t next     = vq->desc[idx].next;
        bool     has_next = (vq->desc[idx].flags & VRING_DESC_F_NEXT) != 0;
        free_desc(vq, idx);
        if (!has_next)
            break;
        idx = next;
    }
}

// ── Ring operations ────────────────────────────────────────────────────

int virtqueue_add_buf(struct virtqueue *vq, uint64_t phys_addr,
                      uint32_t len, bool write, void *data)
{
    int idx = alloc_desc(vq);
    if (idx < 0)
        return -1;

    vq->desc[idx].addr  = phys_addr;
    vq->desc[idx].len   = len;
    vq->desc[idx].flags = write ? VRING_DESC_F_WRITE : 0;
    vq->desc[idx].next  = 0;
    vq->desc_data[idx]  = data;

    // Add to the available ring
    uint16_t avail_idx = vq->avail->idx & (vq->size - 1);
    vq->avail->ring[avail_idx] = (uint16_t)idx;

    // Ensure descriptor is visible before updating avail->idx
    virtio_wmb();
    vq->avail->idx++;

    return idx;
}

int virtqueue_add_chain(struct virtqueue *vq, struct virtio_buf *bufs,
                        int count, void *data)
{
    if (count <= 0 || (uint16_t)count > vq->num_free)
        return -1;

    int head = -1;
    int prev = -1;

    for (int i = 0; i < count; ++i)
    {
        int idx = alloc_desc(vq);
        if (idx < 0)
        {
            // Roll back: free the chain built so far
            if (head >= 0)
            {
                vq->desc[prev].flags &= ~VRING_DESC_F_NEXT;
                free_desc_chain(vq, (uint16_t)head);
            }
            return -1;
        }

        vq->desc[idx].addr  = bufs[i].addr;
        vq->desc[idx].len   = bufs[i].len;
        vq->desc[idx].flags = bufs[i].write ? VRING_DESC_F_WRITE : 0;

        if (prev >= 0)
        {
            vq->desc[prev].flags |= VRING_DESC_F_NEXT;
            vq->desc[prev].next   = (uint16_t)idx;
        }
        else
            head = idx;

        prev = idx;
    }

    vq->desc_data[head] = data;

    // Add the chain head to the available ring
    uint16_t avail_idx = vq->avail->idx & (vq->size - 1);
    vq->avail->ring[avail_idx] = (uint16_t)head;

    virtio_wmb();
    vq->avail->idx++;

    return head;
}

void virtqueue_kick(struct virtqueue *vq)
{
    // Ensure avail->idx update is visible before checking used->flags
    virtio_mb();

    // Only notify if the device hasn't suppressed notifications
    if (!(vq->used->flags & VRING_USED_F_NO_NOTIFY))
        mmio_write16(vq->notify_addr, vq->queue_idx);
}

// ── Completion processing ──────────────────────────────────────────────

void *virtqueue_get_buf(struct virtqueue *vq, uint32_t *len)
{
    virtio_rmb();

    if (vq->last_used_idx == vq->used->idx)
        return NULL;

    uint16_t used_idx = vq->last_used_idx & (vq->size - 1);
    struct vring_used_elem *elem = &vq->used->ring[used_idx];

    uint32_t id = elem->id;
    if (len)
        *len = elem->len;

    void *data = vq->desc_data[id];
    vq->desc_data[id] = NULL;

    // Free the entire descriptor chain
    free_desc_chain(vq, (uint16_t)id);

    vq->last_used_idx++;

    return data;
}

bool virtqueue_has_buf(struct virtqueue *vq)
{
    virtio_rmb();
    return vq->last_used_idx != vq->used->idx;
}

// ── Interrupt control ──────────────────────────────────────────────────

void virtqueue_disable_interrupts(struct virtqueue *vq)
{
    vq->avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
    virtio_wmb();
}

void virtqueue_enable_interrupts(struct virtqueue *vq)
{
    vq->avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
    virtio_wmb();
}

