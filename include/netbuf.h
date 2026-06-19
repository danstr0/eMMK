/**
 * @file netbuf.h
 * @brief Network buffer (netbuf) — eMMK's fundamental packet container.
 *
 * Inspired by Linux's sk_buff, a netbuf wraps a contiguous memory region
 * and maintains four pointers for zero-copy header manipulation:
 *
 * @code
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
#define NETBUF_HEADROOM  64

/// Default total buffer size.
#define NETBUF_SIZE    2048

/// Standard Ethernet MTU (excludes the Ethernet header).
#define NETBUF_MTU     1500
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

    /// @name Addressing (IPv4 only for v0.1)
    /// @{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    /// @}

    /// @name Device and queuing
    /// @{
    struct netdev *dev;   ///< Associated network device.
    struct netbuf *next;  ///< Queue linkage.
    /// @}

    /// Physical address of @c head (for DMA).
    uint64_t phys_addr;

    /// Reference count (freed when it reaches zero).
    int refcount;
};

/// @name Initialization
/// @{

/**
 * @brief Initialize the netbuf subsystem.
 *
 * Creates the slab caches used for netbuf metadata and data buffers.
 * Must be called before any @c netbuf_alloc() calls.
 */
void netbuf_init(void);
/// @}

/// @name Allocation
/// @{

/**
 * @brief Allocate a new network buffer.
 *
 * @param size  Total buffer size in bytes (0 = default @c NETBUF_SIZE).
 *
 * All pointers start at the beginning of the buffer
 * (@c data == @c tail == @c head). Call @c netbuf_reserve() to set
 * up headroom before adding data.
 *
 * @return Pointer to the netbuf, or @c NULL on failure.
 */
struct netbuf *netbuf_alloc(size_t size);

/**
 * @brief Release a network buffer.
 *
 * Decrements the reference count — the buffer is actually freed only
 * when the count reaches zero.
 */
void netbuf_free(struct netbuf *nb);

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

