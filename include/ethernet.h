/**
 * @file ethernet.h
 * @brief Ethernet (Layer 2) frame handling.
 *
 * Handles parsing and construction of Ethernet II frames:
 * @code{.txt}
 *   ┏━━━━━━━━┳━━━━━━━━━┳━━━━━━━━━━━━━━━━━┓
 *   ┃ Offset ┃ Size    ┃ Field           ┃
 *   ┣━━━━━━━━╇━━━━━━━━━╇━━━━━━━━━━━━━━━━━┫
 *   ┃ 0      │ 6       │ Destination MAC ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 6      │ 6       │ Source MAC      ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 12     │ 2       │ EtherType       ┃
 *   ┠────────┼─────────┼─────────────────┨
 *   ┃ 14     │ 46–1500 │ Payload         ┃
 *   ┠────────┴─────────┴─────────────────┨
 *   ┃ (FCS is handled by hardware, not   ┃
 *   ┃ visible to software)               ┃
 *   ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
 * @endcode
 *
 * Minimum frame: 60 bytes (excluding FCS), padded by hardware if needed.
 * Maximum frame: 1514 bytes (excluding FCS).
 *
 * @par Byte-order convention
 * All ethertype @e parameters in this API use host byte order.
 * Conversion to/from network byte order happens inside 
 * @c eth_build_header and @c eth_input. Compare with constants like
 * @c ETH_P_IP directly — no @c htons() needed at the call site.
 *
 * The raw @c ethhdr.ethertype field is in network (big-endian) byte
 * order as it appears on the wire; use @c ntohs() when reading it
 * directly from the struct.
 */

#ifndef EMMK_ETHERNET_H
#define EMMK_ETHERNET_H

#include "netbuf.h"
#include "types.h"

/// @name Constants (bytes)
/// @{
#define ETH_ALEN       6     ///< MAC address length.
#define ETH_HLEN       14    ///< Ethernet header length.
#define ETH_MTU        1500  ///< Maximum payload size.
#define ETH_FRAME_MIN  60    ///< Minimum frame size (excluding FCS).
#define ETH_FRAME_MAX  1514  ///< Maximum frame size (excluding FCS).
/// @}

/// @name Well-known MAC addresses
/// @{

/// Broadcast address: @c FF:FF:FF:FF:FF:FF.
extern const uint8_t ETH_BROADCAST[ETH_ALEN];

/// Zero address: @c 00:00:00:00:00:00 (unset / invalid).
extern const uint8_t ETH_ZERO[ETH_ALEN];
/// @}

/// @name Ethernet header
/// @{

/**
 * @brief Ethernet II frame header (14 bytes).
 *
 * The @c ethertype field is in network (big-endian) byte order on the
 * wire. Use @c ntohs(hdr->ethertype) when reading it directly from
 * the struct. The API functions in this header accept and return
 * ethertype values in @e host byte order.
 */
struct ethhdr
{
    uint8_t  dest[ETH_ALEN];  ///< Destination MAC address.
    uint8_t  src[ETH_ALEN];   ///< Source MAC address.
    uint16_t ethertype;       ///< Protocol type (network byte order).
} PACKED;
/// @}

/// @name Frame processing
/// @{

/**
 * @brief Process a received Ethernet frame.
 *
 * Parses the Ethernet header, sets @c nb->mac_header, determines the
 * packet type (host / broadcast / multicast / other), strips the
 * header (advances @c nb->data past it), and dispatches to the
 * appropriate L3 handler based on ethertype (ARP, IP, etc.).
 *
 * @param nb  Network buffer with @c data pointing at the Ethernet header.
 *
 * @note Takes ownership of @p nb. The buffer is either passed to an
 *       L3 handler or freed (on error or unrecognized ethertype).
 */
void eth_input(struct netbuf *nb);

/**
 * @brief Send an Ethernet frame.
 *
 * Pushes an Ethernet header (using @c nb->dev's MAC as the source),
 * pads to @c ETH_FRAME_MIN if necessary, and transmits the frame via
 * the network device.
 *
 * @param nb         Network buffer with L3 payload.
 *                   @c nb->dev must be set.
 * @param dest       Destination MAC address.
 * @param ethertype  Protocol type in @e host byte order.
 * @return @c 0 on success, @c -1 on failure.
 *
 * @note Always consumes @p nb regardless of success or failure.
 */
int eth_output(struct netbuf *nb, const uint8_t *dest, uint16_t ethertype);

/**
 * @brief Construct an Ethernet header in-place.
 *
 * Pushes @c ETH_HLEN bytes at the front of @p nb and fills in the
 * header. The ethertype is converted to network byte order.
 *
 * @pre @p nb must have ≥ @c ETH_HLEN headroom.
 *
 * @param nb         Network buffer.
 * @param dest       Destination MAC.
 * @param src        Source MAC.
 * @param ethertype  Protocol type in @e host byte order.
 */
void eth_build_header(struct netbuf *nb, const uint8_t *dest,
                      const uint8_t *src, uint16_t ethertype);
/// @}

/// @name MAC address utilities
/// @{

/// Compare two 6-byte MAC addresses for equality.
static inline bool eth_addr_equal(const uint8_t *a, const uint8_t *b)
{
    return (a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2])
        && (a[3] == b[3]) && (a[4] == b[4]) && (a[5] == b[5]);
}

/// Check if a MAC address is the broadcast address (@c FF:FF:FF:FF:FF:FF).
static inline bool eth_is_broadcast(const uint8_t *addr)
{
    return (addr[0] & addr[1] & addr[2]
          & addr[3] & addr[4] & addr[5]) == 0xFF;
}

/**
 * @brief Check if a MAC address is multicast.
 *
 * Bit 0 of the first octet is the I/G (Individual/Group) bit.
 * 
 * @note Broadcast is a special case of multicast.
 */
static inline bool eth_is_multicast(const uint8_t *addr)
{
    return (addr[0] & 0x01) != 0;
}

/// Check if a MAC address is all zeros (unset).
static inline bool eth_is_zero(const uint8_t *addr)
{
    return (addr[0] | addr[1] | addr[2]
          | addr[3] | addr[4] | addr[5]) == 0;
}

/// Copy a 6-byte MAC address from @p src to @p dest.
static inline void eth_copy_addr(uint8_t *dest, const uint8_t *src)
{
    dest[0] = src[0]; dest[1] = src[1]; dest[2] = src[2];
    dest[3] = src[3]; dest[4] = src[4]; dest[5] = src[5];
}

/**
 * @brief Format a MAC address as "xx:xx:xx:xx:xx:xx".
 *
 * @param addr  6-byte MAC address.
 * @param buf   Output buffer (must be ≥ 18 bytes), or @c NULL to use
 *              an internal static buffer.
 * @return @p buf (or the static buffer if @p buf was @c NULL).
 *
 * @warning The static buffer is overwritten on every call with
 *          @p buf == @c NULL. Do @b not use @c ETH_FMT() more than
 *          once in a single @c kprintf argument list — both expansions
 *          return the same pointer and the second call will overwrite
 *          the first. Use explicit buffers instead:
 * @code{.c}
 *   char s[18], d[18];
 *   kprintf("src=%s dst=%s\n",
 *           eth_addr_to_str(hdr->src, s),
 *           eth_addr_to_str(hdr->dest, d));
 * @endcode
 */
char *eth_addr_to_str(const uint8_t *addr, char *buf);

/**
 * @brief Convenience macro — uses the internal static buffer.
 * 
 * @warning Only safe for a @b single MAC in one @c kprintf call.
 * @code{.c}
 *   kprintf("src=%s\n", ETH_FMT(hdr->src));  // OK
 *
 *   kprintf("src=%s dest=%s\n", ETH_FMT(a), ETH_FMT(b));  // BAD
 * @endcode
 */
#define ETH_FMT(addr)  eth_addr_to_str((addr), NULL)
/// @}

#endif // EMMK_ETHERNET_H

