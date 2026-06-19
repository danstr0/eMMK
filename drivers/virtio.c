/**
 * @file virtio.c
 * @brief Virtio transport layer — device init and virtqueue management.
 *
 * Implements the legacy (transitional) virtio PCI interface:
 * - Device initialization: reset, status handshake, feature negotiation.
 * - Virtqueue setup: allocate rings, build free list, write PFN.
 * - Descriptor management: alloc/free from a linked free list.
 * - Ring operations: add buffers, kick device, get completions.
 *
 * @par Memory ordering
 * Virtio requires specific ordering between ring updates and index
 * writes. On x86, stores are naturally ordered (no StoreStore
 * reordering), so a compiler barrier (@c barrier()) suffices before
 * updating @c avail->idx. A compiler barrier also suffices before
 * reading @c used->idx because x86 loads are ordered with respect to
 * other loads.
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

// ── Virtio device setup ────────────────────────────────────────────────

struct virtio_device *virtio_device_init(struct pci_device *pci)
{
    if (!pci)
        return NULL;

    // BAR0 must be an I/O BAR for legacy virtio
    if (!pci->bar[0].is_io || pci->bar[0].base == 0)
    {
        kprintf("[VIRTIO] Device %x:%x.%x: BAR0 is not I/O space\n",
                pci->bus, pci->device, pci->function);
        return NULL;
    }

    struct virtio_device *vdev = kzalloc(sizeof(struct virtio_device));
    if (!vdev)
        return NULL;

    vdev->pci     = pci;
    vdev->io_base = (uint16_t)pci->bar[0].base;

    // Config space offset shifts when MSI-X is active
    vdev->config_offset = pci->cap_msix ? VIRTIO_PCI_CONFIG_MSIX
                                        : VIRTIO_PCI_CONFIG;

    // Enable PCI I/O and bus mastering
    pci_enable_device(pci);

    // Reset the device
    virtio_reset(vdev);

    // Begin the status handshake
    virtio_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(vdev, VIRTIO_STATUS_DRIVER);

    kprintf("[VIRTIO] Initialized device %x:%x.%x, io_base=0x%x\n",
            pci->bus, pci->device, pci->function, vdev->io_base);

    return vdev;
}

uint32_t virtio_negotiate_features(struct virtio_device *vdev,
                                   uint32_t desired)
{
    // Read device-offered features (32-bit, legacy interface)
    uint32_t offered = inl(vdev->io_base + VIRTIO_PCI_HOST_FEATURES);

    // Keep only features both sides support
    uint32_t negotiated = offered & desired;

    // Write back the negotiated set
    outl(vdev->io_base + VIRTIO_PCI_GUEST_FEATURES, negotiated);

    vdev->features = negotiated;

    kprintf("[VIRTIO] Features: offered=0x%lx, desired=0x%lx, "
            "negotiated=0x%lx\n",
            (uint64_t)offered, (uint64_t)desired, (uint64_t)negotiated);

    return negotiated;
}

void virtio_set_status(struct virtio_device *vdev, uint8_t status)
{
    uint8_t current = inb(vdev->io_base + VIRTIO_PCI_DEVICE_STATUS);
    outb(vdev->io_base + VIRTIO_PCI_DEVICE_STATUS, current | status);
}

void virtio_reset(struct virtio_device *vdev)
{
    outb(vdev->io_base + VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_RESET);

    // The device may take time to reset - read back the status until
    // it returns 0 (reset complete).
    while (inb(vdev->io_base + VIRTIO_PCI_DEVICE_STATUS) != 0)
        cpu_relax();
}

uint8_t virtio_read_config8(struct virtio_device *vdev, uint8_t offset)
{
    return inb(vdev->io_base + vdev->config_offset + offset);
}

uint16_t virtio_read_config16(struct virtio_device *vdev, uint8_t offset)
{
    return inw(vdev->io_base + vdev->config_offset + offset);
}

uint32_t virtio_read_config32(struct virtio_device *vdev, uint8_t offset)
{
    return inl(vdev->io_base + vdev->config_offset + offset);
}

// ── Virtqueue creation / destruction ───────────────────────────────────

struct virtqueue *virtqueue_create(struct virtio_device *vdev,
                                   uint16_t queue_idx)
{
    if (queue_idx >= VIRTIO_MAX_QUEUES)
    {
        kprintf("[VIRTIO] Queue index %u exceeds maximum (%u)\n",
                queue_idx, VIRTIO_MAX_QUEUES);
        return NULL;
    }

    // Select the queue and read its size
    outw(vdev->io_base + VIRTIO_PCI_QUEUE_SEL, queue_idx);
    uint16_t size = inw(vdev->io_base + VIRTIO_PCI_QUEUE_SIZE);

    if (size == 0)
    {
        kprintf("[VIRTIO] Queue %u does not exist (size=0)\n", queue_idx);
        return NULL;
    }

    // Size must be a power of 2
    if (size & (size - 1))
    {
        kprintf("[VIRTIO] Queue %u: invalid size %u (not power of 2)\n",
                queue_idx, size);
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

    // Write the queue's physical page frame number to the device.
    // The legacy interface uses a 32-bit PFN (physical address >> 12).
    outl(vdev->io_base + VIRTIO_PCI_QUEUE_PFN,
         (uint32_t)(phys / PAGE_SIZE));

    // Register the queue in the parent device
    vdev->queues[queue_idx] = vq;
    if (queue_idx >= vdev->num_queues)
        vdev->num_queues = queue_idx + 1;

    kprintf("[VIRTIO] Queue %u: size=%u, phys=0x%lx, virt=%p\n",
            queue_idx, size, phys, virt);

    return vq;
}

void virtqueue_destroy(struct virtqueue *vq)
{
    if (!vq)
        return;

    // Tell the device this queue is gone
    if (vq->vdev)
    {
        outw(vq->vdev->io_base + VIRTIO_PCI_QUEUE_SEL, vq->queue_idx);
        outl(vq->vdev->io_base + VIRTIO_PCI_QUEUE_PFN, 0);

        // Unregister from the parent device
        if (vq->queue_idx < VIRTIO_MAX_QUEUES)
            vq->vdev->queues[vq->queue_idx] = NULL;
    }

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
        outw(vq->vdev->io_base + VIRTIO_PCI_QUEUE_NOTIFY, vq->queue_idx);
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

