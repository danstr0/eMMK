/**
 * @file arp.h
 * @brief Address Resolution Protocol (ARP).
 *
 * ARP maps IPv4 addresses to Ethernet MAC addresses on the local
 * network segment. When the IP layer needs to send a packet to a
 * local address:
 * 1. Check the ARP cache for an existing IP → MAC mapping.
 * 2. If found (@c RESOLVED): use the cached MAC address.
 * 3. If not found: broadcast an ARP request, queue the packet,
 *    and return "pending."
 * 4. When the ARP reply arrives: cache the mapping, transmit
 *    all queued packets for that IP.
 *
 * @par ARP packet format (Ethernet + IPv4)
 * @code
 *   ┏━━━━━━━━┳━━━━━━━━━┳━━━━━━━━━━━━━━━━━┓
 *   ┃ Offset ┃ Size    ┃ Field           ┃
 *   ┣━━━━━━━━╇━━━━━━━━━╇━━━━━━━━━━━━━━━━━┫
 *   ┃ 0      │ 2       │ Hardware Type   ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 2      │ 2       │ Protocol Type   ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 4      │ 1       │ Hardware Length ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 5      │ 1       │ Protocol Length ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 6      │ 2       │ Operation       ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 8      │ 6       │ Sender MAC      ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 14     │ 4       │ Sender IP       ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 18     │ 6       │ Target MAC      ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 24     │ 4       │ Target IP       ┃
 *   ┗━━━━━━━━┷━━━━━━━━━┷━━━━━━━━━━━━━━━━━┛
 *   Total: 28 bytes
 * @endcode
 *
 * All multi-byte fields in the ARP header are in network (big-endian)
 * byte order.
 *
 * Cache entries expire after @c ARP_CACHE_TIMEOUT seconds. Call
 * @c arp_cache_expire() periodically (e.g., from a timer callback)
 * to remove stale entries.
 */

#ifndef EMMK_ARP_H
#define EMMK_ARP_H

#include "types.h"
#include "netbuf.h"
#include "ethernet.h"

/// @name Constants
/// @{

/// ARP hardware type for Ethernet.
#define ARP_HW_ETHER         1

/// ARP request operation.
#define ARP_OP_REQUEST       1

/// ARP reply operation.
#define ARP_OP_REPLY         2

/// Maximum entries in the ARP cache.
#define ARP_CACHE_SIZE      64

/// Cache entry lifetime in seconds before expiry.
#define ARP_CACHE_TIMEOUT  300

/// Maximum packets queued per pending ARP entry.
#define ARP_PENDING_MAX      8
/// @}

/// @name ARP cache entry states
/// @{
#define ARP_STATE_FREE      0  ///< Entry is unused.
#define ARP_STATE_PENDING   1  ///< Request sent, awaiting reply.
#define ARP_STATE_RESOLVED  2  ///< Have a valid MAC address.
#define ARP_STATE_STATIC    3  ///< Manually configured (never expires).
/// @}

/// @name Resolution return codes
/// @{
#define ARP_OK       0   ///< Resolved: MAC address written to output.
#define ARP_PENDING  1   ///< Request sent; caller should queue the packet.
#define ARP_ERROR  (-1)  ///< Error (no device, cache full, etc.).
/// @}

/// @name ARP header
/// @{

/**
 * @brief ARP packet layout for Ethernet (HW) + IPv4 (Proto).
 *
 * All multi-byte fields are in @e network byte order. Use
 * @c ntohs() / @c ntohl() when reading, @c htons() / htonl()
 * when writing.
 */
struct arphdr
{
    uint16_t hw_type;               ///< Hardware type (@c ARP_HW_ETHER).
    uint16_t proto_type;            ///< Protocol type (@c ETH_P_IP).
    uint8_t  hw_len;                ///< Hardware address length (6).
    uint8_t  proto_len;             ///< Protocol address length (4).
    uint16_t operation;             ///< @c ARP_OP_REQUEST or @c ARP_OP_REPLY.
    uint8_t  sender_mac[ETH_ALEN];  ///< Sender MAC address.
    uint32_t sender_ip;             ///< Sender IP (network byte order).
    uint8_t  target_mac[ETH_ALEN];  ///< Target MAC address.
    uint32_t target_ip;             ///< Target IP (network byte order).
} PACKED;

#define ARP_HDR_SIZE  sizeof(struct arphdr)  ///< 28 bytes.
/// @}

/// @name ARP cache entry
/// @{
struct arp_entry
{
    uint32_t ip;                  ///< IPv4 address (host byte order).
    uint8_t  mac[ETH_ALEN];       ///< Resolved MAC address.
    uint8_t  state;               ///< @c ARP_STATE_*.
    uint64_t timestamp;           ///< Tick count when the entry was last updated.
    struct netdev *dev;           ///< Associated network device.
    struct netbuf_queue pending;  ///< Packets waiting for resolution.
};
/// @}

/// @name Initialization
/// @{

/// Initialize the ARP cache. Must be called before any ARP operations.
void arp_init(void);
/// @}

/// @name RX path
/// @{

/**
 * @brief Process a received ARP packet.
 *
 * Handles both ARP requests (sends a reply if the target is our IP)
 * and ARP replies (updates the cache, flushes pending packets).
 *
 * @param nb  Network buffer with @c data pointing at the ARP header
 *            (Ethernet header already stripped by @c eth_input).
 *
 * @note Takes ownership of @p nb.
 */
void arp_input(struct netbuf *nb);
/// @}

/// @name Address resolution
/// @{

/**
 * @brief Resolve an IPv4 address to a MAC address.
 *
 * @param dev  Network device to use for the request.
 * @param ip   IPv4 address to resolve (host byte order).
 * @param mac  Output buffer for the MAC address (6 bytes).
 *
 * @retval ARP_OK       Resolved — @p mac is valid.
 * @retval ARP_PENDING  Request sent — caller should queue the packet.
 * @retval ARP_ERROR    Error.
 */
int arp_resolve(struct netdev *dev, uint32_t ip, uint8_t *mac);

/**
 * @brief  Queue a packet pending ARP resolution.
 *
 * The packet is held in the cache entry's pending queue. If the
 * queue is full, the oldest packet is dropped.
 *
 * @param dev  Network device.
 * @param ip   Destination IP (host byte order).
 * @param nb   Packet to queue (sent when the reply arrives).
 * @return @c 0 on success, @c -1 on error.
 */
int arp_queue_packet(struct netdev *dev, uint32_t ip, struct netbuf *nb);

/// Broadcast an ARP request for @p target_ip.
void arp_send_request(struct netdev *dev, uint32_t target_ip);
/// @}

/// @name Cache management
/// @{

/**
 * @brief Look up a MAC address in the cache (no request sent).
 *
 * @param ip   IPv4 address (host byte order).
 * @param mac  Output buffer for MAC address (6 bytes).
 * @return @c 0 if found and resolved, @c -1 if not in cache.
 */
int arp_lookup(uint32_t ip, uint8_t *mac);

/**
 * @brief Add a permanent (non-expiring) ARP entry.
 *
 * @param ip   IPv4 address (host byte order).
 * @param mac  MAC address (6 bytes).
 * @param dev  Associated network device.
 */
void arp_add_static(uint32_t ip, const uint8_t *mac, struct netdev *dev);

/**
 * @brief Remove stale cache entries.
 *
 * Walks the cache and frees entries older than @c ARP_CACHE_TIMEOUT
 * seconds. Static entries are never expired. Pending entries that
 * have timed out have their queued packets dropped.
 *
 * @note Call periodically (e.g., every 30 seconds from a timer).
 */
void arp_cache_expire(void);
/// @}

/// @name Debug
/// @{

/// Dump the ARP cache to serial output.
void arp_cache_dump(void);
/// @}

#endif // EMMK_ARP_H

