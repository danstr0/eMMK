/**
 * @file udp.h
 * @brief User Datagram Protocol (UDP).
 *
 * Provides connectionless, unreliable datagram delivery with port-based
 * demultiplexing. The API is callback-driven: the developer binds a
 * handler function to a port, and incoming datagrams for that port
 * invoke the handler directly from the receive path.
 *
 * @par Header format (8 bytes)
 * @code
 *   ┏━━━━━━━━┳━━━━━━━━━┳━━━━━━━━━━━━━━━━━━━━┓
 *   ┃ Offset ┃ Size    ┃ Field              ┃
 *   ┣━━━━━━━━╇━━━━━━━━━╇━━━━━━━━━━━━━━━━━━━━┫
 *   ┃ 0      │ 16 bits │ Source Port        ┃
 *   ┠────────┼─────────┼────────────────────┨
 *   ┃ 2      │ 16 bits │ Destination Port   ┃
 *   ┠────────┼─────────┼────────────────────┨
 *   ┃ 4      │ 16 bits │ Length             ┃
 *   ┠────────┼─────────┼────────────────────┨
 *   ┃ 6      │ 16 bits │ Checksum           ┃
 *   ┗━━━━━━━━┷━━━━━━━━━┷━━━━━━━━━━━━━━━━━━━━┛
 * @endcode
 *
 * All multi-byte fields are in network byte order. The checksum covers
 * a pseudo-header (src IP, dst IP, protocol, UDP length) plus the
 * UDP header and payload. A transmitted checksum of 0 means "not
 * computed" (valid for IPv4).
 *
 * @par Callback ownership
 * The handler registered via @c udp_bind() takes ownership of the
 * @c netbuf. When the handler is called, @c nb->data points to the
 * UDP payload (header already stripped) and the addressing fields
 * (@c src_ip, @c dst_ip, @c src_port, @c dst_port) are populated.
 * The handler must call @c netbuf_free() when done.
 *
 * @par Usage
 * @code{.c}
 *   static void my_handler(struct netbuf *nb)
 *   {
 *       // nb->data = payload
 *       // nb->len  = payload length
 *       // nb->src_ip, nb->src_port = sender
 *       process(nb->data, nb->len);
 *       netbuf_free(nb);
 *   }
 *
 *   udp_bind(12345, my_handler);
 *   udp_send(dst_ip, 12345, 54321, "hello", 5);
 * @endcode
 */

#ifndef EMMK_UDP_H
#define EMMK_UDP_H

#include "netbuf.h"
#include "types.h"

/// @name UDP header
/// @{

/**
 * @brief UDP datagram header (8 bytes).
 *
 * All fields are in network byte order.
 */
struct udphdr
{
    uint16_t src_port;  ///< Source port.
    uint16_t dst_port;  ///< Destination port.
    uint16_t len;       ///< Header + payload length.
    uint16_t checksum;  ///< Checksum (@c 0 = not computed).
} PACKED;

#define UDP_HDR_SIZE  sizeof(struct udphdr)  ///< 8 bytes.
/// @}

/// @name Configuration
/// @{

/// Maximum simultaneous port bindings.
#define UDP_MAX_BINDS  32
/// @}

/// @name Callback
/// @{

/**
 * @brief UDP receive callback.
 *
 * Invoked when a datagram arrives for a bound port. The handler
 * takes ownership of @p nb and must free it when done.
 *
 * @param nb  Network buffer with @c data pointing at the UDP payload,
 *            addressing fields populated.
 */
typedef void (*udp_recv_func_t)(struct netbuf *nb);
/// @}

/// @name Initialization
/// @{

/// Initialize the UDP subsystem (clears the bind table).
void udp_init(void);
/// @}

/// @name Port binding
/// @{

/**
 * @brief Register a handler for incoming datagrams on a port.
 *
 * @param port     Port number (host byte order).
 * @param handler  Callback invoked on receipt (takes ownership of @c nb).
 * @return @c 0 on success, @c -1 if the port is already bound or the
 *         table is full.
 */
int udp_bind(uint16_t port, udp_recv_func_t handler);

/**
 * @brief Remove the handler for a port.
 *
 * Datagrams arriving for this port after unbinding will trigger an
 * ICMP Port Unreachable response.
 *
 * @param port  Port number (host byte order).
 */
void udp_unbind(uint16_t port);
/// @}

/// @name RX path
/// @{

/**
 * @brief Process a received UDP datagram.
 *
 * Validates the header and checksum, looks up the destination port
 * in the bind table, and dispatches to the registered handler. If
 * no handler is bound, sends an ICMP Port Unreachable and frees the
 * buffer.
 *
 * @param nb  Network buffer with @c data pointing at the UDP header
 *            (IP header already stripped by @c ip_input()).
 *
 * @note Takes ownership of @p nb.
 */
void udp_input(struct netbuf *nb);
/// @}

/// @name TX path
/// @{

/**
 * @brief Send a UDP datagram (copy interface).
 *
 * Allocates a @c netbuf, copies the payload, builds the UDP and IP
 * headers, computes the checksum, and transmits.
 *
 * @param dst_ip    Destination IP (host byte order).
 * @param dst_port  Destination port (host byte order).
 * @param src_port  Source port (host byte order).
 * @param data      Payload data (may be @c NULL if @p len == 0).
 * @param len       Payload length in bytes.
 * @return @c 0 on success, @c -1 on error.
 */
int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const void *data, size_t len);

/**
 * @brief Send a UDP datagram (zero-copy interface).
 *
 * The caller has already placed the payload in @p nb via
 * @c netbuf_put(). This function pushes the UDP header, computes
 * the checksum, and passes the buffer to @c ip_output().
 *
 * @param nb        Network buffer with payload. Must have
 *                  @c ETH_HLEN + @c IP_HDR_MIN + @c UDP_HDR_SIZE
 *                  bytes of headroom.
 * @param dst_ip    Destination IP (host byte order).
 * @param dst_port  Destination port (host byte order).
 * @param src_port  Source port (host byte order).
 * @return @c 0 on success (or queued for ARP), @c -1 on error.
 *
 * @note Always consumes @p nb.
 */
int udp_output(struct netbuf *nb, uint32_t dst_ip,
               uint16_t dst_port, uint16_t src_port);
/// @}

#endif // EMMK_UDP_H

