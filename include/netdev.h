/**
 * @file netdev.h
 * @brief Network device abstraction layer.
 *
 * Provides a unified interface for network devices (virtio-net, e1000,
 * etc.) using an operations table (@c netdev_ops) for device-specific
 * behavior.
 *
 * Each network device has:
 * - Identity: name, MAC address.
 * - IP configuration: address, netmask, gateway (IPv4 only).
 * - Operations: open, close, send, poll (provided by the driver).
 * - Statistics: packet / byte counters.
 * - RX queue: incoming packets queued for stack processing.
 *
 * @par Lifecycle
 * 1. Driver allocates a netdev (@c netdev_alloc).
 * 2. Driver fills in name, MAC, ops, priv.
 * 3. Driver calls @c netdev_register().
 * 4. Network stack calls @c netdev_set_ip() to configure addressing.
 * 5. Main loop calls @c netdev_poll_all() + @c netdev_process_rx().
 */

#ifndef EMMK_NETDEV_H
#define EMMK_NETDEV_H

#include "types.h"
#include "netbuf.h"
#include "string.h"

// ETH_ALEN is defined in ethernet.h, but including it here would
// create a circular dependency
#ifndef ETH_ALEN
#define ETH_ALEN  6
#endif

/// @name Configuration
/// @{

/// Maximum number of registered network devices.
#define NETDEV_MAX  4
/// @}

/// @name Device flags
/// @{
#define NETDEV_F_UP         (1 << 0)  ///< Administratively up.
#define NETDEV_F_RUNNING    (1 << 1)  ///< Link up and running.
#define NETDEV_F_PROMISC    (1 << 2)  ///< Promiscuous mode.
#define NETDEV_F_MULTICAST  (1 << 3)  ///< Multicast reception.
#define NETDEV_F_BROADCAST  (1 << 4)  ///< Broadcast reception.
/// @}

/// @name Statistics
/// @{
struct netdev_stats
{
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
};
/// @}

/// @name Device operations
/// @{

struct netdev;

/**
 * @brief Driver-specific operations for a network device.
 *
 * Filled in by the device driver and set as @c dev->ops during
 * registration. Function pointers may be @c NULL if the operation
 * is not supported.
 */
struct netdev_ops
{
    /// Bring the device up (enable RX / TX).
    int  (*open)(struct netdev *dev);

    /// Bring the device down (disable RX / TX).
    int  (*close)(struct netdev *dev);

    /// Transmit a packet. The driver takes ownership of @p nb.
    int  (*send)(struct netdev *dev, struct netbuf *nb);

    /// Poll for received packets (called from the main loop).
    void (*poll)(struct netdev *dev);

    /// Set the device's MAC address.
    int  (*set_mac)(struct netdev *dev, const uint8_t *mac);
};
/// @}

/// Network device structure.
struct netdev
{
    char     name[16];       ///< Device name (e.g., "eth0").
    uint8_t  mac[ETH_ALEN];  ///< Hardware MAC address.
    uint32_t flags;          ///< @c NETDEV_F_* flags.
    uint32_t mtu;            ///< Maximum transmission unit.

    /// @name IPv4 configuration
    /// @{
    uint32_t ip_addr;  ///< IP address (host byte order).
    uint32_t netmask;  ///< Subnet mask (host byte order).
    uint32_t gateway;  ///< Default gateway (host byte order).
    /// @}

    struct netdev_stats stats;

    /// @name Driver interface
    /// @{
    const struct netdev_ops *ops;  ///< Device operations.
    void *priv;                    ///< Driver-private data.
    /// @}

    /// Receive queue (packets waiting for stack processing).
    struct netbuf_queue rx_queue;
};

/// @name Initialization
/// @{

/// Initialize the network device subsystem.
void netdev_init(void);

/**
 * @brief Allocate and initialize a new netdev.
 *
 * Sets default MTU, zeroes stats, and initializes the RX queue. The
 * caller must fill in @c mac, @c ops, and @c priv before registering.
 *
 * @param name  Device name (copied, max 15 chars + NUL).
 * @return Pointer to the new netdev, or @c NULL on failure.
 */
struct netdev *netdev_alloc(const char *name);
/// @}

/// @name Registration
/// @{

/**
 * @brief Register a network device with the subsystem.
 * @return 0 on success, -1 if the device table is full.
 */
int netdev_register(struct netdev *dev);

/// Remove a device from the subsystem.
void netdev_unregister(struct netdev *dev);
/// @}

/// @name Lookup
/// @{

/// Find a device by name. Returns @c NULL if not found.
struct netdev *netdev_find(const char *name);

/// Get the first registered device (default). Returns @c NULL if none.
struct netdev *netdev_get_default(void);
/// @}

/// @name Data path
/// @{

/**
 * @brief Transmit a packet through a device.
 *
 * Calls @c dev->ops->send(). Always consumes @p nb regardless of
 * success or failure (freed on error, owned by the driver on success).
 *
 * @param dev  Network device.
 * @param nb   Network buffer.
 * @return 0 on success, -1 on failure.
 */
int netdev_send(struct netdev *dev, struct netbuf *nb);

/**
 * @brief Queue a received packet for stack processing.
 *
 * Called by the device driver (from poll or IRQ handler) to hand a
 * received packet to the network stack. The packet is queued and
 * processed during @c netdev_process_rx().
 *
 * @param dev  Network device that received the packet.
 * @param nb   Network buffer containing the raw frame.
 */
void netdev_receive(struct netdev *dev, struct netbuf *nb);

/// Poll a single device for received packets.
void netdev_poll(struct netdev *dev);

/// Poll all registered devices.
void netdev_poll_all(void);

/**
 * @brief Process all queued received packets.
 *
 * Dequeues packets from each device's RX queue and passes them to
 * @c eth_input() for L2 processing. Called from the main
 * run-to-completion loop.
 */
void netdev_process_rx(void);
/// @}

/// @name Configuration
/// @{

/**
 * @brief Configure IPv4 addressing for a device.
 *
 * @param dev      Network device.
 * @param ip       IP address (host byte order).
 * @param netmask  Subnet mask (host byte order).
 * @param gateway  Default gateway (host byte order).
 */
void netdev_set_ip(struct netdev *dev, uint32_t ip, uint32_t netmask,
                   uint32_t gateway);
/// @}

/// @name Inline helpers
/// @{

/// Copy the device's MAC address into @p mac (6 bytes).
static inline void netdev_get_mac(struct netdev *dev, uint8_t *mac)
{
    memcpy(mac, dev->mac, ETH_ALEN);
}

/// Get the device's IP address (host byte order).
static inline uint32_t netdev_get_ip(struct netdev *dev)
{
    return dev->ip_addr;
}

/// Get the device's subnet mask (host byte order).
static inline uint32_t netdev_get_netmask(struct netdev *dev)
{
    return dev->netmask;
}

/// Get the device's default gateway (host byte order).
static inline uint32_t netdev_get_gateway(struct netdev *dev)
{
    return dev->gateway;
}
/// @}

/// @name Flag helpers
/// @{
static inline bool netdev_is_up(struct netdev *dev)
{
    return (dev->flags & NETDEV_F_UP) != 0;
}

static inline void netdev_set_up(struct netdev *dev)
{
    dev->flags |= NETDEV_F_UP | NETDEV_F_BROADCAST;
}

static inline void netdev_set_down(struct netdev *dev)
{
    dev->flags &= ~(NETDEV_F_UP | NETDEV_F_RUNNING);
}

static inline bool netdev_is_running(struct netdev *dev)
{
    return (dev->flags & NETDEV_F_RUNNING) != 0;
}
/// @}

/// @name IP address formatting
/// @{

/**
 * @brief Format an IPv4 address as "A.B.C.D".
 *
 * @param ip   IPv4 address in host byte order.
 * @param buf  Output buffer (≥ 16 bytes), or @c NULL for an internal
 *             static buffer.
 * @return @p buf (or the static buffer).
 *
 * @warning The static buffer is overwritten on every call with
 *          @p buf == @c NULL. Do not use @c IP4_FMT() more than once
 *          in a single @c kprintf argument list.
 */
char *ip4_addr_to_str(uint32_t ip, char *buf);

/// Convenience macro for @c kprintf (uses the internal static buffer).
#define IP4_FMT(ip)  ip4_addr_to_str((ip), NULL)

/**
 * @brief Construct a host-order IPv4 address from four octets.
 *
 * @code
 *   uint32_t ip = IP4_ADDR(192, 168, 1, 1);
 * @endcode
 */
#define IP4_ADDR(a, b, c, d)                       \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) \
   | ((uint32_t)(c) << 8)  | (uint32_t)(d))
/// @}

#endif // EMMK_NETDEV_H

