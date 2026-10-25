/**
 * @file ip.h
 * @brief Internet Protocol version 4 (IPv4).
 *
 * Provides IP packet parsing, construction, checksum computation,
 * and basic routing (local subnet vs gateway).
 *
 * @par IPv4 header (20 bytes minimum, no options)
 * @code
 *   0               1               2               3
 *   0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 *  ┏━━━━━━━┯━━━━━━━┯━━━━━━━━━━━━━━━┯━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
 *  ┃Version│  IHL  │      TOS      │          Total Length         ┃
 *  ┠───────┴───────┴───────────────┼─────┬─────────────────────────┨
 *  ┃         Identification        │Flags│    Fragment Offset      ┃
 *  ┠───────────────┬───────────────┼─────┴─────────────────────────┨
 *  ┃      TTL      │   Protocol    │       Header Checksum         ┃
 *  ┠───────────────┴───────────────┴───────────────────────────────┨
 *  ┃                       Source Address                          ┃
 *  ┠───────────────────────────────────────────────────────────────┨
 *  ┃                    Destination Address                        ┃
 *  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
 * @endcode
 *
 * All multi-byte fields are in @e network byte order.
 *
 * @par Byte-order convention
 * IP addresses passed to / from API functions are in @e host byte order.
 * The @c iphdr struct fields are in network byte order (wire format).
 * Use @c ntohl() / @c htonl() when converting between the two.
 */

#ifndef EMMK_IP_H
#define EMMK_IP_H

#include "netbuf.h"
#include "types.h"

/// @name Constants
/// @{
#define IP_VERSION       4  ///< IPv4.
#define IP_HDR_MIN      20  ///< Minimum header length (no options).
#define IP_MAX_SIZE  65535  ///< Maximum IP datagram size.
#define IP_DEFAULT_TTL  64  ///< Default Time To Live.
/// @}

/// @name IP header flags and fragment offset
/// @{

/**
 * The @c frag_off field packs flags (bits 15:13) and fragment offset
 * (bits 12:0). These constants are in host byte order — apply after 
 * @c ntohs(hdr->frag_off).
 */
#define IP_FLAG_DF    0x4000  ///< Don't Fragment.
#define IP_FLAG_MF    0x2000  ///< More Fragments.
#define IP_FRAG_MASK  0x1FFF  ///< Fragment offset (in 8-byte units).
/// @}

/// @name IPv4 header
/// @{

/**
 * @brief IPv4 packet header (20 bytes, no options).
 *
 * All multi-byte fields are in network byte order.
 * Bitfield ordering depends on host endianness.
 */
struct iphdr
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint8_t  ihl:4;      ///< Header length in 32-bit words (min 5).
    uint8_t  version:4;  ///< IP version (4).
#else
    uint8_t  version:4;
    uint8_t  ihl:4;
#endif
    uint8_t  tos;        ///< Type of Service / DSCP + ECN.
    uint16_t tot_len;    ///< Total length (header + payload).
    uint16_t id;         ///< Identification (for fragmentation).
    uint16_t frag_off;   ///< Flags (3 bits) + Fragment offset (13).
    uint8_t  ttl;        ///< Time To Live (hop count).
    uint8_t  protocol;   ///< L4 protocol (@c IPPROTO_*).
    uint16_t check;      ///< Header checksum.
    uint32_t saddr;      ///< Source IP (network byte order).
    uint32_t daddr;      ///< Destination IP (network order).
    // Options follow if ihl > 5
} PACKED;
/// @}

/// @name Initialization
/// @{

/// Initialize the IP subsystem (counters, ID generator).
void ip_init(void);
/// @}

/// @name RX path
/// @{

/**
 * @brief Process a received IP packet.
 *
 * Validates the header (version, IHL, checksum, length), sets
 * @c nb->network_header, trims Ethernet padding, and dispatches to
 * the L4 handler based on the protocol field (ICMP, TCP, UDP).
 *
 * @param nb  Network buffer with @c data pointing at the IP header.
 *
 * @note No forwarding — packets not addressed to this host are dropped.
 * @note Takes ownership of @p nb.
 */
void ip_input(struct netbuf *nb);
/// @}

/// @name TX path
/// @{

/**
 * @brief Send an IP packet to a destination.
 *
 * Builds the IP header, determines the route (local vs gateway),
 * resolves the next-hop MAC via ARP, and calls @c eth_output().
 * If ARP resolution is pending, the packet is queued and sent
 * automatically when the reply arrives.
 *
 * @param nb        Network buffer with L4 payload.
 * @param dst_ip    Destination IP address (host byte order).
 * @param protocol  L4 protocol number (@c IPPROTO_*).
 * @return @c 0 on success (or queued), @c -1 on error.
 *
 * @note Always consumes @p nb.
 */
int ip_output(struct netbuf *nb, uint32_t dst_ip, uint8_t protocol);

/**
 * @brief Send an IP packet with a known destination MAC.
 *
 * Called by the ARP layer when a pending resolution completes.
 * Passes the packet directly to @c eth_output() without re-routing.
 *
 * @param nb       Network buffer (IP header already at @c nb->data).
 * @param dst_mac  Resolved destination MAC address.
 * @return @c 0 on success, @c -1 on error.
 */
int ip_output_resolved(struct netbuf *nb, const uint8_t *dst_mac);
/// @}

/// @name Transmit ownership (single-queue NIC)
///
/// With one TX virtqueue and one ARP cache, only one core may resolve
/// next hops and transmit. This is an interim measure; with multiqueue
/// this handoff disappears.
/// @{

/// Ring size for each core's TX handoff ring.
#define IP_TX_RING_SIZE  256

/**
 * @brief Make @p core the only core that transmits.
 *
 * Creates a handoff ring for every other online core. Call before the
 * other cores start sending (i.e., before @c smp_launch_all()). Pass
 * @c -1 to return to direct transmission from every core.
 */
void ip_set_tx_owner(int core);

/**
 * @brief Finish packets handed off by other cores (owner core only).
 * @return Number of packets processed.
 */
size_t ip_tx_drain(void);

/// @return Packets currently waiting in all handoff rings.
size_t ip_tx_pending(void);

/// @return Packets dropped because a handoff ring was full.
uint64_t ip_tx_handoff_drops(void);

/// @name Checksum
/// @{

/**
 * @brief Compute the ones-complement checksum.
 *
 * Used for the IP header checksum and as a building block for TCP / UDP
 * pseudo-header checksums.
 *
 * @param data  Data to checksum.
 * @param len   Length in bytes.
 * @return 16-bit ones-complement checksum (host byte order).
 */
uint16_t ip_checksum(const void *data, size_t len);
/// @}

/// @name Routing
/// @{

/**
 * @brief Determine the outgoing device and next-hop for a destination.
 *
 * @param dst_ip         Destination IP (host byte order).
 * @param[out] dev       Outgoing network device.
 * @param[out] next_hop  Next-hop IP for ARP resolution (may be the gateway).
 *
 * If @p dst_ip is on the local subnet, @p next_hop = @p dst_ip.
 * Otherwise, @p next_hop = default gateway.
 *
 * @return @c 0 on success, @c -1 if no route (no device or no gateway).
 */
int ip_route(uint32_t dst_ip, struct netdev **dev, uint32_t *next_hop);
/// @}

/// @name Address classification
/// @{

/**
 * @brief Check if an IP address belongs to this host.
 *
 * @param ip  IPv4 address (host byte order).
 * @return @c true if @p ip matches any registered device's IP or is
 *         a loopback address (127.x.x.x).
 */
bool ip_is_local(uint32_t ip);

/**
 * @brief Check if an IP is a broadcast address.
 *
 * @param ip   IPv4 address (host byte order).
 * @param dev  Network device (for subnet-directed broadcast check).
 * @return @c true for 255.255.255.255 and for the subnet-directed
 *         broadcast address of @p dev.
 */
bool ip_is_broadcast(uint32_t ip, struct netdev *dev);
/// @}

#endif // EMMK_IP_H

