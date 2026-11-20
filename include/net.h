/**
 * @file net.h
 * @brief Stack-wide services: per-core counters and control-core
 *        housekeeping.
 *
 * @par Counters
 * The data path never prints. Every drop, and every protocol event worth
 * knowing about, increments a named per-core counter instead
 * (@c net_stat_inc()): one cache line block per core, written only by
 * that core, so incrementing costs one non-atomic add and causes no
 * cache-line traffic. Totals are summed on demand (@c net_stat_total())
 * from any core; a total read while other cores run is approximate
 * (each counter is exact, they are just not read at one instant).
 *
 * The counters are declared once, in @c NET_STAT_LIST, with their
 * printable names, so code that inspects them (@c net_stats_dump(), and
 * later introspection tools) enumerates them instead of knowing them.
 *
 * @par Housekeeping
 * Protocol timers (ARP retransmission and aging) run on the control core,
 * from its poll loop: @c net_housekeeping() is cheap to call on every
 * iteration and does real work every @c NET_HOUSEKEEPING_MS. Before an
 * idle wait, the loop asks @c net_timers_pending(): while a timer is
 * armed, it must keep polling rather than halt (the kernel is tickless,
 * so nothing would wake it to retransmit).
 */

#ifndef EMMK_NET_H
#define EMMK_NET_H

#include "types.h"
#include "percpu.h"
#include "smp.h"

/// @name Counter list
/// @{

/**
 * @brief Every network counter: `X(identifier, "printable name")`.
 *
 * Add a counter by adding a line here; @c NET_STAT_<identifier> is its
 * index, and the name is what reports print.
 */
#define NET_STAT_LIST(X)                                                  \
    /* Ethernet */                                                        \
    X(ETH_RUNT,           "eth_runt")           /* shorter than header */ \
    X(ETH_OTHERHOST,      "eth_otherhost")      /* unicast, not to us  */ \
    X(ETH_UNKNOWN_TYPE,   "eth_unknown_type")   /* not ARP / IPv4      */ \
    X(ETH_ARP_MISROUTED,  "eth_arp_misrouted")  /* ARP off the owner   */ \
    /* ARP */                                                             \
    X(ARP_RX_REQUEST,     "arp_rx_request")                               \
    X(ARP_RX_REPLY,       "arp_rx_reply")                                 \
    X(ARP_TX_REQUEST,     "arp_tx_request")     /* incl. retries       */ \
    X(ARP_TX_REPLY,       "arp_tx_reply")                                 \
    X(ARP_RETRANSMIT,     "arp_retransmit")                               \
    X(ARP_MALFORMED,      "arp_malformed")                                \
    X(ARP_IGNORED,        "arp_ignored")        /* failed validation   */ \
    X(ARP_CONFLICT,       "arp_conflict")       /* claims our address  */ \
    X(ARP_RESOLVE_FAILED, "arp_resolve_failed") /* no reply: gave up   */ \
    X(ARP_EXPIRED,        "arp_expired")        /* refresh unanswered  */ \
    X(ARP_QUEUE_DROP,     "arp_queue_drop")     /* waiting pkt dropped */ \
    X(ARP_CACHE_FULL,     "arp_cache_full")     /* no slot to evict    */ \
    /* IPv4 */                                                            \
    X(IP_SHORT,           "ip_short")                                     \
    X(IP_BAD_VERSION,     "ip_bad_version")                               \
    X(IP_BAD_HDR_LEN,     "ip_bad_hdr_len")                               \
    X(IP_BAD_CSUM,        "ip_bad_csum")                                  \
    X(IP_BAD_LEN,         "ip_bad_len")         /* total length        */ \
    X(IP_NOT_LOCAL,       "ip_not_local")       /* not ours: no fwd    */ \
    X(IP_UNKNOWN_PROTO,   "ip_unknown_proto")                             \
    X(IP_NO_ROUTE,        "ip_no_route")                                  \
    /* ICMP */                                                            \
    X(ICMP_RX_ECHO_REQ,   "icmp_rx_echo_request")                         \
    X(ICMP_RX_ECHO_REPLY, "icmp_rx_echo_reply")                           \
    X(ICMP_RX_UNREACH,    "icmp_rx_dest_unreachable")                     \
    X(ICMP_RX_TIME_EXC,   "icmp_rx_time_exceeded")                        \
    X(ICMP_RX_OTHER,      "icmp_rx_other")                                \
    X(ICMP_SHORT,         "icmp_short")                                   \
    X(ICMP_BAD_CSUM,      "icmp_bad_csum")                                \
    X(ICMP_TX_ECHO_REQ,   "icmp_tx_echo_request")                         \
    X(ICMP_TX_ECHO_REPLY, "icmp_tx_echo_reply")                           \
    X(ICMP_TX_UNREACH,    "icmp_tx_dest_unreachable")                     \
    /* UDP */                                                             \
    X(UDP_SHORT,          "udp_short")                                    \
    X(UDP_BAD_LEN,        "udp_bad_len")                                  \
    X(UDP_BAD_CSUM,       "udp_bad_csum")                                 \
    X(UDP_NO_PORT,        "udp_no_port")

/// Counter indices: @c NET_STAT_ETH_RUNT, ...
enum net_stat
{
#define NET_STAT_ENUM(id, name) NET_STAT_##id,
    NET_STAT_LIST(NET_STAT_ENUM)
#undef NET_STAT_ENUM
    NET_STAT_COUNT  ///< Number of counters.
};
/// @}

/// @name Per-core counters
/// @{

/**
 * @brief One core's counters, on cache lines of their own.
 *
 * Only the owning core writes its block; any core may read.
 */
struct net_core_stats
{
    uint64_t c[NET_STAT_COUNT];  ///< Indexed by `enum net_stat`.
} CACHE_ALIGNED;

/// Counter blocks, indexed by core.
extern struct net_core_stats net_core_stats[SMP_MAX_CORES];

/**
 * @brief Count one event on the calling core.
 *
 * One %gs load and one add; safe on any core, interrupts on or off
 * (device interrupt handlers do not count network events).
 *
 * @param s  Counter to increment.
 */
static inline void net_stat_inc(enum net_stat s)
{
    net_core_stats[this_core_id()].c[s]++;
}

/**
 * @brief Sum a counter over all cores.
 * @param s  Counter.
 * @return Its total (approximate while other cores are counting).
 */
uint64_t net_stat_total(enum net_stat s);

/**
 * @brief Printable name of a counter.
 * @param s  Counter.
 * @return Its name from @c NET_STAT_LIST, or @c "?" if out of range.
 */
const char *net_stat_name(enum net_stat s);

/**
 * @brief Print every non-zero counter total, then a per-core breakdown
 *        of the ones that are non-zero on more than one core.
 *
 * @note Prints: call it only from a core that is not on the data path
 *       (or accept the stall, e.g., in a periodic report).
 */
void net_stats_dump(void);

/// Zero every counter on every core (tests only: not safe while other
/// cores count).
void net_stats_reset(void);
/// @}

/// @name Housekeeping (control core)
/// @{

/// Interval between housekeeping passes, in milliseconds.
#define NET_HOUSEKEEPING_MS  100

/**
 * @brief Run due protocol timers (ARP retransmission and aging).
 *
 * Call from the control core's poll loop on every iteration; it returns
 * immediately unless @c NET_HOUSEKEEPING_MS have passed since the last
 * pass. Must run on the core that owns the ARP cache (the TX owner, or
 * the only core polling).
 */
void net_housekeeping(void);

/**
 * @brief Is a protocol timer armed (an ARP request awaiting its reply)?
 *
 * While true, the control core must not halt in an idle wait: nothing
 * would wake it to retransmit.
 *
 * @todo Arm a one-shot LAPIC (TSC-deadline) interrupt for the next timer
 *       instead, so an idle control core can halt.
 */
bool net_timers_pending(void);
/// @}

#endif // EMMK_NET_H

