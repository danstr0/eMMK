/**
 * @file arp.h
 * @brief Address Resolution Protocol (ARP).
 *
 * ARP maps IPv4 addresses to Ethernet MAC addresses on the local
 * network segment. When the IP layer needs to send a packet to a
 * neighbor on the interface @e dev:
 * 1. Look up the (@e dev, IP) entry in the cache.
 * 2. If it has a MAC (@c REACHABLE, @c STALE, or @c STATIC): use it.
 * 3. If not: create an @c INCOMPLETE entry, broadcast a request, queue
 *    the packet on the entry, and return "pending".
 * 4. When the reply arrives: record the MAC, transmit the queued
 *    packets.
 *
 * @par Entry life cycle (driven by @c arp_timer() on the control core)
 * @code{.txt}
 *              request sent                   reply
 *   (none) ──────────────────► INCOMPLETE ──────────────► REACHABLE
 *                              │  retransmit every            │
 *                              │  ARP_RETRANS_MS, up to       │ ARP_REACHABLE_MS
 *                              │  ARP_MAX_PROBES requests     │ since confirmed
 *                              ▼                              ▼
 *                    freed, queued packets         STALE: MAC still used,
 *                    dropped (arp_resolve_failed)  refresh requests sent;
 *                                                  reply → REACHABLE,
 *                                                  no reply → freed (arp_expired)
 * @endcode
 * @c STATIC entries never change and never expire.
 *
 * Entries are keyed by (interface, IPv4 address): an ARP frame received
 * on one interface never touches another interface's entries.
 *
 * @par Ownership
 * The cache belongs to one core, the TX owner (@c ip_set_tx_owner()), or
 * to the only core polling when there is none. All functions here run
 * on that core.
 *
 * @par ARP packet format (Ethernet + IPv4)
 * @code{.txt}
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
 */

#ifndef EMMK_ARP_H
#define EMMK_ARP_H

#include "types.h"
#include "netbuf.h"
#include "ethernet.h"

struct netdev;

/// @name Constants
/// @{

/// ARP hardware type for Ethernet.
#define ARP_HW_ETHER          1

/// ARP request operation.
#define ARP_OP_REQUEST        1

/// ARP reply operation.
#define ARP_OP_REPLY          2

/// Maximum entries in the ARP cache.
#define ARP_CACHE_SIZE       64

/// A confirmed entry is trusted for this long, then refreshed (ms).
#define ARP_REACHABLE_MS  60000

/// Interval between requests for one address (ms).
#define ARP_RETRANS_MS     1000

/// Requests sent for one resolution or refresh before giving up.
#define ARP_MAX_PROBES        3

/// Maximum packets queued per incomplete entry.
#define ARP_PENDING_MAX      8
/// @}

/// @name ARP cache entry states
/// @{
#define ARP_STATE_FREE        0  ///< Entry is unused.
#define ARP_STATE_INCOMPLETE  1  ///< Request sent, no MAC yet.
#define ARP_STATE_REACHABLE   2  ///< MAC confirmed recently.
#define ARP_STATE_STALE       3  ///< MAC in use, refresh outstanding.
#define ARP_STATE_STATIC      4  ///< Configured; never changes or expires.
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

/// One neighbor: (interface, IPv4 address) → MAC.
struct arp_entry
{
    struct netdev *dev;           ///< Interface (part of the key).
    uint32_t ip;                  ///< IPv4 address, host order (key).
    uint8_t  mac[ETH_ALEN];       ///< MAC (valid unless @c INCOMPLETE).
    uint8_t  state;               ///< @c ARP_STATE_*.
    uint8_t  probes;              ///< Requests sent in this resolution / refresh.
    uint64_t updated;             ///< Last confirmation, or creation (ms).
    uint64_t deadline;            ///< Next request due (ms; INCOMPLETE / STALE).
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
 * Validates the frame (malformed → `arp_malformed`; a broadcast,
 * multicast or zero sender MAC, or a sender outside the interface's
 * subnet → `arp_ignored`; a sender claiming our address → `arp_conflict`),
 * refreshes the sender's entry on this interface if there is one, and, if
 * the frame targets our address: answers a request and records the sender.
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
 * Starts resolution (an @c INCOMPLETE entry and one broadcast request)
 * when there is no entry; retransmission is @c arp_timer()'s job.
 *
 * @param dev  Interface the neighbor is on.
 * @param ip   IPv4 address to resolve (host byte order).
 * @param mac  Output buffer for the MAC address (6 bytes).
 *
 * @retval ARP_OK       Resolved — @p mac is valid.
 * @retval ARP_PENDING  Resolution under way — queue the packet.
 * @retval ARP_ERROR    No device, or no free cache slot.
 */
int arp_resolve(struct netdev *dev, uint32_t ip, uint8_t *mac);

/**
 * @brief Queue a packet until its next hop resolves.
 *
 * Starts resolution if needed. If the entry's queue is full, the oldest
 * packet is dropped (@c arp_queue_drop). If the address resolves, the
 * packet is sent at once.
 *
 * @param dev  Interface.
 * @param ip   Next hop (host byte order).
 * @param nb   Packet (sent when the reply arrives).
 * @return @c 0 if queued or sent, @c -1 if dropped.
 *
 * @note Takes ownership of @p nb.
 */
int arp_queue_packet(struct netdev *dev, uint32_t ip, struct netbuf *nb);

/// Broadcast an ARP request for @p target_ip on @p dev.
void arp_send_request(struct netdev *dev, uint32_t target_ip);
/// @}

/// @name Timers
/// @{

/**
 * @brief Run ARP's timers: retransmit, give up, refresh, expire.
 *
 * Called by @c net_housekeeping(); tests call it with a made-up clock.
 *
 * @param now_ms  Current time (@c timer_get_ms()).
 */
void arp_timer(uint64_t now_ms);

/// @return @c true while any resolution is incomplete (requests are
///         still to be retransmitted).
bool arp_timers_pending(void);
/// @}

/// @name Cache management
/// @{

/**
 * @brief Look up a MAC address in the cache (no request sent).
 *
 * @param dev  Interface.
 * @param ip   IPv4 address (host byte order).
 * @param mac  Output buffer for the MAC address (6 bytes).
 * @return @c 0 if the entry has a MAC, @c -1 otherwise.
 */
int arp_lookup(struct netdev *dev, uint32_t ip, uint8_t *mac);

/**
 * @brief State of the (@p dev, @p ip) entry.
 * @return @c ARP_STATE_*, @c ARP_STATE_FREE if there is none.
 */
int arp_entry_state(struct netdev *dev, uint32_t ip);

/**
 * @brief Add a permanent entry (never refreshed, never expires, never
 *        overwritten by received ARP).
 *
 * @param ip   IPv4 address (host byte order).
 * @param mac  MAC address (6 bytes).
 * @param dev  Interface.
 */
void arp_add_static(uint32_t ip, const uint8_t *mac, struct netdev *dev);

/**
 * @brief Drop every entry of @p dev (and the packets queued on them).
 *
 * Called by @c netdev_unregister(): no entry may outlive its interface.
 */
void arp_flush_dev(struct netdev *dev);
/// @}

/// @name Debug
/// @{

/// Dump the ARP cache to serial output (prints: not on a data-path core).
void arp_cache_dump(void);
/// @}

#endif // EMMK_ARP_H

