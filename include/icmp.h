/**
 * @file icmp.h
 * @brief Internet Control Message Protocol (ICMP).
 *
 * ICMP provides network-layer diagnostics and error reporting:
 * - Echo Request / Reply (ping).
 * - Destination Unreachable.
 * - Time Exceeded (not implemented — no forwarding).
 *
 * @par Header format (8 bytes)
 * @code{.txt}
 *   ┏━━━━━━━━┳━━━━━━━━━┳━━━━━━━━━━━━━━━━━━━━┓
 *   ┃ Offset ┃ Size    ┃ Field              ┃
 *   ┣━━━━━━━━╇━━━━━━━━━╇━━━━━━━━━━━━━━━━━━━━┫
 *   ┃ 0      │ 1       │ Type               ┃
 *   ┠────────┼─────────┼────────────────────┨
 *   ┃ 1      │ 1       │ Code               ┃
 *   ┠────────┼─────────┼────────────────────┨
 *   ┃ 2      │ 2       │ Checksum           ┃
 *   ┠────────┼─────────┼────────────────────┨
 *   ┃ 4      │ 4       │ Type-specific data ┃
 *   ┗━━━━━━━━┷━━━━━━━━━┷━━━━━━━━━━━━━━━━━━━━┛
 * @endcode
 *
 * All multi-byte fields are in network byte order. The checksum
 * covers the header + payload.
 */

#ifndef EMMK_ICMP_H
#define EMMK_ICMP_H

#include "netbuf.h"
#include "types.h"

/// @name ICMP message types
/// @{
#define ICMP_ECHO_REPLY          0  ///< Echo Reply (ping response).
#define ICMP_DEST_UNREACHABLE    3  ///< Destination Unreachable.
#define ICMP_SOURCE_QUENCH       4  ///< Source Quench (deprecated).
#define ICMP_REDIRECT            5  ///< Redirect.
#define ICMP_ECHO_REQUEST        8  ///< Echo Request (ping).
#define ICMP_TIME_EXCEEDED      11  ///< Time Exceeded (TTL expired).
#define ICMP_PARAMETER_PROBLEM  12  ///< Parameter Problem.
#define ICMP_TIMESTAMP          13  ///< Timestamp Request.
#define ICMP_TIMESTAMP_REPLY    14  ///< Timestamp Reply.
/// @}

/// @name ICMP codes for Destination Unreachable (type 3)
/// @{
#define ICMP_NET_UNREACHABLE    0  ///< Network unreachable.
#define ICMP_HOST_UNREACHABLE   1  ///< Host unreachable.
#define ICMP_PROTO_UNREACHABLE  2  ///< Protocol unreachable.
#define ICMP_PORT_UNREACHABLE   3  ///< Port unreachable.
#define ICMP_FRAG_NEEDED        4  ///< Frag needed but DF set (+ next MTU).
#define ICMP_SR_FAILED          5  ///< Source route failed.
/// @}

/// @name ICMP codes for Time Exceeded (type 11)
/// @{
#define ICMP_TTL_EXCEEDED  0  ///< TTL expired in transit.
#define ICMP_FRAG_TIMEOUT  1  ///< Fragment reassembly timeout.
/// @}

/// @name ICMP header
/// @{

/**
 * @brief ICMP message header (8 bytes).
 *
 * The type-specific data field is a union covering common formats:
 * - @c echo:    ID + sequence number (ping).
 * - @c gateway: redirect gateway address.
 * - @c frag:    next-hop MTU (for @c ICMP_FRAG_NEEDED).
 */
struct icmphdr
{
    uint8_t  type;      ///< ICMP message type.
    uint8_t  code;      ///< Type-specific code.
    uint16_t checksum;  ///< Ones-complement checksum (hdr + payload).
    union
    {
        struct
        {
            uint16_t id;        ///< Echo identifier.
            uint16_t sequence;  ///< Echo sequence number.
        } echo;
        uint32_t gateway;       ///< Redirect: gateway IP (network order).
        struct
        {
            uint16_t unused;
            uint16_t mtu;       ///< Next-hop MTU (network byte order).
        } frag;
        uint32_t raw;           ///< Generic access to the 4-byte field.
    } un;
} PACKED;

#define ICMP_HDR_SIZE  sizeof(struct icmphdr)  ///< 8 bytes.
/// @}

/// @name Initialization
/// @{

/// Initialize ICMP (reset statistics).
void icmp_init(void);
/// @}

/// @name RX path
/// @{

/**
 * @brief Process a received ICMP packet.
 *
 * Validates the checksum, then dispatches by type:
 * - Echo Request: sends an Echo Reply.
 * - Echo Reply: logs.
 * - Other: logs and drops.
 *
 * @param nb  Network buffer with @c data pointing at the ICMP header
 *            (IP header already stripped by @c ip_input).
 */
void icmp_input(struct netbuf *nb);
/// @}

/// @name TX — echo (ping)
/// @{

/**
 * @brief Reply to an echo request.
 *
 * Modifies the ICMP header in place (type → Reply, recomputes
 * checksum), then sends the packet back to the source via
 * @c ip_output().
 *
 * @param nb  The original Echo Request packet (ICMP header + payload).
 *
 * @note Takes ownership of @p nb.
 */
void icmp_send_echo_reply(struct netbuf *nb);

/**
 * @brief Send a ping (Echo Request).
 *
 * @param dst_ip  Destination IP (host byte order).
 * @param id      Echo identifier (host byte order).
 * @param seq     Echo sequence number (host byte order).
 * @param data    Optional payload data (may be @c NULL if @p len == @c 0).
 * @param len     Payload length in bytes.
 * @return @c 0 on success, @c -1 on error.
 */
int icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq,
                           const void *data, size_t len);
/// @}

/// @name TX — error messages
/// @{

/**
 * @brief Send a Destination Unreachable message.
 *
 * Per RFC 792, the error message includes the original IP header
 * plus the first 8 bytes of the original payload.
 *
 * @param nb    The original IP packet that triggered the error.
 *              @c nb->network_header must point at the original IP
 *              header.
 * @param code  ICMP unreachable code (@c ICMP_*_UNREACHABLE).
 *
 * @note Does @b not take ownership of @p nb — the caller is
 *       responsible for freeing it.
 */
void icmp_send_dest_unreachable(struct netbuf *nb, uint8_t code);
/// @}

/// @name Echo replies (application hook)
/// @{

/**
 * @brief Called for every received echo reply.
 * @param src_ip  Replying host (host byte order).
 * @param id      Echo identifier.
 * @param seq     Echo sequence number.
 */
typedef void (*icmp_echo_reply_hook_t)(uint32_t src_ip, uint16_t id,
                                       uint16_t seq);

/**
 * @brief Install (or, with @c NULL, remove) the echo-reply hook.
 *
 * The stack itself never prints; an application that sends pings and
 * wants to see the replies (e.g., the default kernel's gateway ping)
 * installs a hook. It runs on the core that received the reply, inside
 * the packet path, so it must be as cheap as that core can afford.
 */
void icmp_set_echo_reply_hook(icmp_echo_reply_hook_t hook);
/// @}

/// @name Statistics
/// @{

/// ICMP totals.
struct icmp_stats
{
    uint64_t rx_echo_request;      ///< Echo requests received.
    uint64_t rx_echo_reply;        ///< Echo replies received.
    uint64_t tx_echo_request;      ///< Echo requests sent.
    uint64_t tx_echo_reply;        ///< Echo replies sent.
    uint64_t tx_dest_unreachable;  ///< Destination Unreachable sent.
    uint64_t rx_errors;            ///< Messages shorter than a header.
    uint64_t checksum_errors;      ///< Messages with a bad checksum.
};

/// Sum the ICMP counters of all cores into @p stats.
void icmp_get_stats(struct icmp_stats *stats);
/// @}

#endif // EMMK_ICMP_H

