/**
 * @file netbuf.h
 * @brief Network buffer (netbuf) — eMMK's fundamental packet container.
 *
 * Inspired by Linux's sk_buff, a netbuf wraps a contiguous memory region
 * and maintains four pointers for zero-copy header manipulation:
 *
 * @code{.txt}
 *   head               data              tail               end
 *   |----headroom------|----payload------|----tailroom------|
 * @endcode
 *
 * As a packet moves through the stack:
 * - @b RX: @c data starts at the Ethernet header; @c pull / @c push
 *   adjust as headers are parsed and stripped.
 * - @b TX: reserve headroom first, then @c put the payload, then
 *   @c push each header layer (transport → network → Ethernet)
 *   without copying.
 *
 * @par Memory layout (one allocation per packet)
 * Metadata is embedded at the front of each 2 KiB buffer (DPDK mbuf
 * style). Below @c head lies a small device headroom that the stack never
 * sees, so a driver can always prepend its header (virtio-net: 12 bytes)
 * to any netbuf without copying:
 * @code{.txt}
 *   | struct netbuf | device headroom | head ............... end |
 *   0              128               144                     2048
 * @endcode
 *
 * @par Where netbufs come from
 * - @b Pools (@c netbuf_pool_create()): one per RX queue, carved from a
 *   2 MiB-aligned block the direct map covers with a large page. The
 *   owner core allocates and frees through a LIFO stack without locks or
 *   atomics; a free on any other core goes through a lock-free list that
 *   the owner reclaims. Used for every received packet.
 * - @b Slab (@c netbuf_alloc()): everything else (locally built packets).
 *   Usable from any core, and not meant for a per-packet fast path.
 * - @b PMM: @c netbuf_alloc() of more than @c NETBUF_DATA_SIZE bytes.
 *
 * @par Lifecycle
 *   1. @c netbuf_alloc(size)          — allocate buffer + metadata.
 *   2. @c netbuf_reserve(nb,headroom) — skip space for headers (TX).
 *   3. @c netbuf_put(nb,len)          — append payload data.
 *   4. @c netbuf_push(nb,len)         — prepend a header.
 *   5. ... pass through the stack ...
 *   6. @c netbuf_free(nb)             — release when done.
 *
 * @note IPv6 is not supported in v0.1. The @c src_ip / @c dst_ip
 *       fields are IPv4-only (@c uint32_t). @c ETH_P_IPV6 is defined
 *       for future use.
 */

#ifndef EMMK_NETBUF_H
#define EMMK_NETBUF_H

#include "serial.h"
#include "types.h"

/// @name Configuration
/// @{

/// Default headroom reserved for TX packets (ETH + IP + TCP).
#define NETBUF_HEADROOM           64

/// Total size of a standard netbuf: metadata, device headroom, and data.
#define NETBUF_SIZE             2048

/// Bytes reserved at the front of the buffer for `struct netbuf`.
#define NETBUF_META_SIZE         128

/// Headroom below @c head, for driver headers (never used by the stack).
#define NETBUF_DEV_HEADROOM       16

/// Offset of @c head from the start of the buffer.
#define NETBUF_DATA_OFFSET      (NETBUF_META_SIZE + NETBUF_DEV_HEADROOM)

/// Usable bytes between @c head and @c end of a standard netbuf.
#define NETBUF_DATA_SIZE        (NETBUF_SIZE - NETBUF_DATA_OFFSET)

/// Standard Ethernet MTU (excludes the Ethernet header).
#define NETBUF_MTU              1500

/// Size and alignment of a pool's backing block (one large page).
#define NETBUF_POOL_BLOCK_SIZE  (2UL * 1024 * 1024)

/// Upper bound on the buffers in one pool.
#define NETBUF_POOL_MAX         (NETBUF_POOL_BLOCK_SIZE / NETBUF_SIZE)
/// @}

/// @name Protocol constants
/// @{

// Ethertype values (host byte order).
#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
#define ETH_P_IPV6  0x86DD

// IP protocol numbers
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP   17

// Packet types (determined during RX)
#define PKT_HOST       0  ///< Addressed to this host.
#define PKT_BROADCAST  1  ///< Broadcast.
#define PKT_MULTICAST  2  ///< Multicast.
#define PKT_OTHERHOST  3  ///< Not for this host (promiscuous capture).
/// @}

/// Network buffer structure
struct netbuf
{
    /// @name Buffer region pointers
    /// @{
    uint8_t *head;  ///< Start of allocated buffer (never changes).
    uint8_t *data;  ///< Start of current packet data.
    uint8_t *tail;  ///< End of current packet data.
    uint8_t *end;   ///< End of allocated buffer (never changes).
    /// @}

    /// @name Packet metadata
    /// @{
    uint32_t len;       ///< Data length: @c tail - @c data.
    uint16_t protocol;  ///< L3 protocol (@c ETH_P_*).
    uint8_t  pkt_type;  ///< Packet type (@c PKT_*).
    uint8_t  ip_proto;  ///< L4 protocol (@c IPPROTO_*).
    /// @}

    /// @name Layer header pointers (set during parsing)
    /// @{
    void *mac_header;        ///< Ethernet header.
    void *network_header;    ///< IP header.
    void *transport_header;  ///< TCP / UDP / ICMP header.
    /// @}

    /// @name Addressing (IPv4 only for now)
    /// @{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t next_hop;
    /// @}

    /// @name Device and queuing
    /// @{
    struct netdev *dev;   ///< Associated network device.
    struct netbuf *next;  ///< Queue linkage.
    /// @}

    /// Physical address of @c head (for DMA).
    uint64_t phys_addr;

    /// Pool this buffer returns to, or @c NULL (slab / PMM).
    struct netbuf_pool *pool;

    /// Reference count (freed when it reaches zero).
    int refcount;
};

_Static_assert(sizeof(struct netbuf) <= NETBUF_META_SIZE,
               "struct netbuf must fit in NETBUF_META_SIZE");

/**
 * @brief A per-queue pool of netbufs in one 2 MiB block.
 *
 * Resides at the start of its own block. @c stack is touched only by the
 * owner core, so allocation and same-core free are plain array accesses.
 */
struct netbuf_pool
{
    int      owner_core;    ///< Core that allocates from this pool.
    uint32_t capacity;      ///< Buffers in the pool.
    uint32_t free_count;    ///< Entries in @c stack.
    uint64_t phys;          ///< Physical address of the block.

    /// Buffers freed by other cores (lock-free list through @c nb->next);
    /// the owner takes the whole list at once.
    struct netbuf *remote_free;

    uint64_t allocs;        ///< Successful allocations (owner only).
    uint64_t alloc_fails;   ///< Allocations that found the pool empty.
    uint64_t remote_frees;  ///< Frees from other cores.

    struct netbuf *stack[NETBUF_POOL_MAX];  ///< LIFO of free buffers.
};

/// @name Initialization
/// @{

/**
 * @brief Initialize the netbuf subsystem.
 *
 * Creates the slab cache used by @c netbuf_alloc(). Must be called before
 * any @c netbuf_alloc() or @c netbuf_pool_create() calls.
 */
void netbuf_init(void);
/// @}

/// @name Allocation
/// @{

/**
 * @brief Allocate a new network buffer (slab, or PMM if oversized).
 *
 * Usable from any core. Counted by @c netbuf_slow_allocs(): the data
 * path allocates from a pool instead.
 *
 * @param size  Usable bytes between @c head and @c end (@c 0 = default
 *              @c NETBUF_DATA_SIZE).
 *
 * All pointers start at the beginning of the buffer
 * (`data == tail == head`). Call @c netbuf_reserve() to set up headroom
 * before adding data.
 *
 * @return Pointer to the netbuf, or @c NULL on failure.
 */
struct netbuf *netbuf_alloc(size_t size);

/**
 * @brief Release a network buffer.
 *
 * Decrements the reference count — the buffer is actually freed only
 * when the count reaches zero. Callable on any core, whatever the
 * buffer's origin.
 */
void netbuf_free(struct netbuf *nb);

/// @return Number of slab / PMM allocations so far (the slow path). An
///         echoed or forwarded packet adds none.
uint64_t netbuf_slow_allocs(void);

/**
 * @brief Create a deep copy of @p nb.
 *
 * Allocates a new buffer, copies the data region, and duplicates all
 * metadata (including remapped header pointers). The copy is
 * independent — modifying one does not affect the other.
 *
 * @return New @c netbuf, or @c NULL on failure.
 */
struct netbuf *netbuf_copy(struct netbuf *nb);
/// @}

/// @name Pools
/// @{

/**
 * @brief Create a pool from a fresh 2 MiB-aligned PMM block.
 *
 * @param owner_core  The only core that may call @c netbuf_pool_alloc().
 * @return The pool (all buffers free), or @c NULL if no block is free.
 */
struct netbuf_pool *netbuf_pool_create(int owner_core);

/**
 * @brief Return a pool's block to the PMM.
 * @warning Every buffer dies with it: the caller guarantees that nothing
 *          (including a device) still refers to one.
 */
void netbuf_pool_destroy(struct netbuf_pool *pool);

/**
 * @brief Take a buffer from @p pool (owner core only).
 *
 * The buffer is reset: `data == tail == head`, `refcount 1`. Data bytes
 * are not cleared. @c phys_addr is fixed at pool creation.
 *
 * @return The buffer, or @c NULL if every buffer is in use.
 */
struct netbuf *netbuf_pool_alloc(struct netbuf_pool *pool);

/**
 * @brief Free buffers in @p pool (owner core only).
 *
 * Reclaims buffers other cores have freed.
 */
uint32_t netbuf_pool_available(struct netbuf_pool *pool);
/// @}

/// @name Device access
/// @{

/// Physical address of @c nb->data, for DMA.
static inline uint64_t netbuf_data_phys(const struct netbuf *nb)
{
    return nb->phys_addr + (uint64_t)(nb->data - nb->head);
}
/// @}

/// @name Headroom / tailroom queries
/// @{
static inline size_t netbuf_headroom(struct netbuf *nb)
{
    return (size_t)(nb->data - nb->head);
}

static inline size_t netbuf_tailroom(struct netbuf *nb)
{
    return (size_t)(nb->end - nb->tail);
}
/// @}

/// @name Headroom / tailroom management
/// @{

/**
 * @brief Reserve headroom at the start of an empty buffer.
 *
 * Advances both @c data and @c tail past the reserved region. Call
 * immediately after @c netbuf_alloc(), before adding any data.
 *
 * @param nb   Network buffer (must be empty: @c data == @c tail).
 * @param len  Bytes of headroom to reserve.
 */
static inline void netbuf_reserve(struct netbuf *nb, size_t len)
{
    if (UNLIKELY(nb->data + len > nb->end))
    {
        kprintf("[NETBUF] reserve overflow: %lu > tailroom %lu\n",
                (uint64_t)len, (uint64_t)(nb->end - nb->data));
        return;
    }
    nb->data += len;
    nb->tail += len;
}

/**
 * @brief Extend the data region at the tail.
 *
 * @param nb   Network buffer.
 * @param len  Bytes to add.
 * @return Pointer to the start of the newly added region, or @c NULL
 *         if there is not enough tailroom.
 */
static inline void *netbuf_put(struct netbuf *nb, size_t len)
{
    if (UNLIKELY(nb->tail + len > nb->end))
    {
        kprintf("[NETBUF] put overflow: %lu > tailroom %lu\n",
                (uint64_t)len, (uint64_t)(nb->end - nb->tail));
        return NULL;
    }
    void *p = nb->tail;
    nb->tail += len;
    nb->len  += len;
    return p;
}

/**
 * @brief @c netbuf_put() that fails silently (device-supplied lengths on
 *        the data path, where a bad frame must not reach the console).
 * @return @c true if the region was added.
 */
static inline bool netbuf_put_quiet(struct netbuf *nb, size_t len)
{
    if (UNLIKELY(len > (size_t)(nb->end - nb->tail)))
        return false;
    nb->tail += len;
    nb->len  += len;
    return true;
}

/**
 * @brief Prepend header space at the front.
 *
 * @param nb   Network buffer.
 * @param len  Bytes to prepend.
 * @return Pointer to the new data start (the header region), or
 *         @c NULL if there is not enough headroom.
 */
static inline void *netbuf_push(struct netbuf *nb, size_t len)
{
    if (UNLIKELY(nb->data - len < nb->head))
    {
        kprintf("[NETBUF] push underflow: %lu > headroom %lu\n",
                (uint64_t)len, (uint64_t)(nb->data - nb->head));
        return NULL;
    }
    nb->data -= len;
    nb->len  += len;
    return nb->data;
}

/**
 * @brief Remove data from the front (strip a header).
 *
 * @param nb   Network buffer.
 * @param len  Bytes to remove.
 * @return Pointer to the old data start (the stripped header), or
 *         @c NULL if @p len exceeds the current data length.
 */
static inline void *netbuf_pull(struct netbuf *nb, size_t len)
{
    if (UNLIKELY(len > nb->len))
    {
        kprintf("[NETBUF] pull underflow: %lu > len %lu\n",
                (uint64_t)len, (uint64_t)nb->len);
        return NULL;
    }
    void *p = nb->data;
    nb->data += len;
    nb->len  -= len;
    return p;
}

/**
 * @brief Remove data from the tail.
 *
 * Useful for stripping Ethernet padding or truncating to IP total
 * length.
 *
 * @param nb   Network buffer.
 * @param len  Bytes to remove from the end.
 */
static inline void netbuf_trim(struct netbuf *nb, size_t len)
{
    if (UNLIKELY(len > nb->len))
    {
        kprintf("[NETBUF] trim underflow: %lu > len %lu\n",
                (uint64_t)len, (uint64_t)nb->len);
        return;
    }
    nb->tail -= len;
    nb->len  -= len;
}

/**
 * @brief Set the data length directly (adjusts @c tail).
 *
 * @param nb      Network buffer.
 * @param new_len Desired data length.
 */
static inline void netbuf_set_len(struct netbuf *nb, uint32_t new_len)
{
    if (UNLIKELY(nb->data + new_len > nb->end))
    {
        kprintf("[NETBUF] set_len overflow: %u > available %lu\n",
                new_len, (uint64_t)(nb->end - nb->data));
        return;
    }
    nb->tail = nb->data + new_len;
    nb->len  = new_len;
}
/// @}

/// @name Packet queue
/// @{

/**
 * @brief Simple singly-linked FIFO queue for netbufs.
 * 
 * @warning Not thread-safe.
 */
struct netbuf_queue
{
    struct netbuf *head;
    struct netbuf *tail;
    uint32_t count;
};

static inline void netbuf_queue_init(struct netbuf_queue *q)
{
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
}

static inline bool netbuf_queue_empty(struct netbuf_queue *q)
{
    return q->head == NULL;
}

static inline uint32_t netbuf_queue_len(struct netbuf_queue *q)
{
    return q->count;
}

/// Enqueue @p nb at the tail.
void netbuf_queue_push(struct netbuf_queue *q, struct netbuf *nb);

/// Dequeue a packet from the head. Returns @c NULL if empty.
struct netbuf *netbuf_queue_pop(struct netbuf_queue *q);

/// Free all packets in @p q.
void netbuf_queue_purge(struct netbuf_queue *q);
/// @}

#endif // EMMK_NETBUF_H

