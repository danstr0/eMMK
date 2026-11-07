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
 * - Operations: open, close, per-queue RX / TX bursts (provided by the
 *   driver).
 * - Hardware queues: @c num_queues RX/TX pairs, each used by one core.
 * - Statistics: packet / byte counters.
 *
 * @par Lifecycle
 * 1. Driver allocates a netdev (@c netdev_alloc).
 * 2. Driver fills in name, MAC, ops, priv.
 * 3. Driver calls @c netdev_register().
 * 4. Network stack calls @c netdev_set_ip() to configure addressing.
 * 5. Main loop calls @c netdev_poll_all(), which receives a burst from
 *    every device and runs each packet through the stack.
 *
 * @par Bursts
 * @c netdev_rx_burst() returns netbufs whose data @c is the DMA buffer;
 * @c netdev_tx_burst() posts netbufs directly. Each rings at most one
 * doorbell. A stack reply built while a burst is being processed is held
 * back (see @c netdev_tx_defer_begin()) so that the whole burst of
 * replies also shares one doorbell.
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
#define NETDEV_MAX  16

/// Largest burst handled at once (and TX packets held back per device).
#define NETDEV_BURST_MAX  32
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

    /**
     * Receive up to @p max packets from queue @p qid. Each returned
     * netbuf holds one Ethernet frame (@c data at its first byte) and
     * belongs to the caller. Refills the ring; at most one doorbell.
     * @return Number of packets stored in @p pkts.
     */
    uint16_t (*rx_burst)(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t max);

    /**
     * Post the first packets of @p pkts on queue @p qid; at most one
     * doorbell. The driver takes ownership of those it accepts, and frees
     * them on completion; it must not touch the rest. Also reclaims
     * completed packets, so `n == 0` is a pure reclaim.
     * @return Number of packets accepted.
     */
    uint16_t (*tx_burst)(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t n);

    /**
     * Allow (@p enable) or suppress the interrupt of RX queue @p qid.
     * Suppressed is the default: polling cores never take it. After
     * enabling, the caller must poll once more before halting.
     */
    void (*rx_irq)(struct netdev *dev, uint16_t qid, bool enable);

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
    uint16_t num_queues;           ///< Hardware RX/TX queue pairs.
    /// @}

    /// @name Held-back transmissions (see @c netdev_tx_defer_begin())
    /// @{
    uint16_t tx_held;                          ///< Packets in @c tx_hold.
    struct netbuf *tx_hold[NETDEV_BURST_MAX];  ///< Packets not yet posted.
    /// @}
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
 * 
 * A device with an empty name gets the lowest unused @c ethN.
 *
 * @return @c 0 on success, @c -1 if the device table is full or the name
 *         is already taken.
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

/// @return Number of registered devices.
int netdev_count(void);

/**
 * @return The @p index-th registered device (registration order), or
 *         @c NULL.
 */
struct netdev *netdev_get(int index);
/// @}

/// @name Data path
/// @{

/**
 * @brief Receive a burst of packets from one queue.
 *
 * Sets @c nb->dev and updates the RX counters.
 *
 * @param dev   Network device.
 * @param qid   Queue index (below @c dev->num_queues).
 * @param pkts  Output array.
 * @param max   Capacity of @p pkts.
 * @return Number of packets received (each now owned by the caller).
 */
uint16_t netdev_rx_burst(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t max);

/**
 * @brief Transmit a burst of packets on one queue, with one doorbell.
 *
 * @param dev   Network device.
 * @param qid   Queue index (below @c dev->num_queues).
 * @param pkts  Packets to send.
 * @param n     Number of packets (@c 0 = only reclaim completed packets).
 * @return Number of packets accepted (now owned by the driver). The
 *         remaining ones still belong to the caller.
 */
uint16_t netdev_tx_burst(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t n);

/**
 * @brief Transmit a packet through a device (stack entry point).
 *
 * Always consumes @p nb regardless of success or failure (freed on
 * error, owned by the driver on success). Inside a
 * @c netdev_tx_defer_begin() section the packet is held back and posted
 * with the rest of the burst; a later failure is then counted in
 * @c tx_dropped, and @c 0 has been returned.
 *
 * @param dev  Network device.
 * @param nb   Network buffer.
 * @return @c 0 on success, @c -1 on failure.
 */
int netdev_send(struct netdev *dev, struct netbuf *nb);

/**
 * @brief Start holding back @c netdev_send() on every device.
 *
 * Lets a burst of replies share one doorbell per device. Only the TX
 * owner may use it. @c netdev_tx_defer_end() posts what is held.
 */
void netdev_tx_defer_begin(void);

/// Stop holding back, and post everything held on every device.
void netdev_tx_defer_end(void);

/**
 * @brief Receive one burst from queue 0 and run it through the stack.
 * @return Number of packets processed.
 */
uint16_t netdev_poll(struct netdev *dev);

/**
 * @brief Poll every device, with replies batched per device.
 * @return Number of packets processed.
 */
uint32_t netdev_poll_all(void);

/**
 * @brief Allow or suppress RX interrupts on every device.
 *
 * Interrupts are suppressed by default: a core that polls continuously
 * is never disturbed. See @c netdev_idle_wait().
 */
void netdev_rx_irq_all(bool enable);

/**
 * @brief Sleep until a device interrupt, without losing a wake-up.
 *
 * Enables RX interrupts, polls once more (a packet that arrived before
 * they were enabled raises no interrupt), and halts only if that found
 * nothing. Interrupts are suppressed again on return.
 *
 * @pre Interrupts disabled on this core (they stay disabled on return).
 */
void netdev_idle_wait(void);
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

