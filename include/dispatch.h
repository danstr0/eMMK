/**
 * @file dispatch.h
 * @brief Software Receive Side Scaling — per-core packet distribution.
 *
 * Distributes incoming packets across worker cores using a flow hash,
 * emulating hardware RSS without requiring multi-queue NIC support.
 *
 * @par Architecture
 * @code{.txt}
 *   ┌─────────────┐     ┌───────────┐     ┌──────────────┐
 *   │ virtio-net  │────▶│ rx_queue  │────▶│ distributor  │
 *   │ (poll)      │     │ (netdev)  │     │ (core 0)     │
 *   └─────────────┘     └───────────┘     └──┬───┬───┬───┘
 *                                            │   │   │
 *                          ┌─────────────────┘   │   └─────────────────┐
 *                          │ SPSC                │ SPSC           SPSC │
 *                          ▼                     ▼                     ▼
 *                    ┌──────────┐          ┌──────────┐          ┌──────────┐
 *                    │ worker 0 │          │ worker 1 │          │ worker 2 │
 *                    │ (core 1) │          │ (core 2) │          │ (core 3) │
 *                    └──────────┘          └──────────┘          └──────────┘
 * @endcode
 *
 * The distributor core polls the NIC, dequeues received packets from the
 * netdev RX queue, extracts the 5-tuple <code>(src_ip, dst_ip, src_port,
 * dst_port, protocol)</code>, hashes it to a worker index, and enqueues
 * the packet on that worker's lock-free SPSC ring.
 *
 * Each worker core polls its SPSC ring and processes packets through the
 * full network stack (@c eth_input → @c ip_input → protocol handler). No
 * locks are needed on the data path.
 *
 * Non-IP traffic (ARP) is processed directly by the distributor to
 * keep the ARP cache single-threaded.
 *
 * @par Usage
 * @code{.c}
 *   int workers[] = {1, 2, 3};
 *   dispatch_init(workers, 3);
 *
 *   smp_assign_role(0, dispatch_distributor_loop, "distributor");
 *   smp_assign_role(1, dispatch_worker_loop, "worker-0");
 *   smp_assign_role(2, dispatch_worker_loop, "worker-1");
 *   smp_assign_role(3, dispatch_worker_loop, "worker-2");
 *   smp_launch_all();
 * @endcode
 */

#ifndef EMMK_DISPATCH_H
#define EMMK_DISPATCH_H

#include "types.h"
#include "spsc.h"
#include "netbuf.h"
#include "smp.h"

/// @name Configuration
/// @{

/// Maximum number of worker cores.
#define DISPATCH_MAX_WORKERS  (SMP_MAX_CORES - 1)

/// Default SPSC ring size per worker (power of 2).
#define DISPATCH_RING_SIZE    256
/// @}

/// @name Statistics
/// @{

/**
 * @brief Dispatch subsystem counters.
 *
 * @c distributed and @c direct are updated by the distributer core;
 * @c processed is the sum across all workers. All are lock-free
 * (per-core or single-writer).
 */
struct dispatch_stats
{
    uint64_t distributed;  ///< Packets enqueued to workers.
    uint64_t direct;       ///< Non-IP packets processed by distributor.
    uint64_t drops;        ///< Packets dropped (ring full).
};
/// @}

/// @name Initialization
/// @{

/**
 * @brief Configure the dispatch system.
 *
 * Creates a SPSC ring for each worker core. Must be called before
 * @c smp_launch_all().
 *
 * @param worker_cores  Array of core indices designated as workers.
 * @param num_workers   Number of entries in @p worker_cores.
 */
void dispatch_init(const int *worker_cores, int num_workers);
/// @}

/// @name Core loops (@c core_run_func_t)
/// @{

/**
 * @brief Distributor loop — polls the NIC and distributes. 
 *
 * Polls all network devices, dequeues received packets from each
 * device's RX queue, hashes them to a worker, and enqueues on that
 * worker's SPSC ring. Non-IP packets (ARP) are handled directly.
 *
 * @param core_index  The core running this loop (typically @c 0).
 * @note Never returns.
 */
NORETURN void dispatch_distributor_loop(int core_index);

/**
 * @brief Worker loop — drains the core's SPSC ring.
 *
 * Dequeues packets from the ring and passes each to @c eth_input()
 * for full stack processing.
 *
 * @param core_index  The core running this loop.
 * @note Never returns.
 */
NORETURN void dispatch_worker_loop(int core_index);
/// @}

/// @name Internals (for custom loops)
/// @{

/**
 * @brief Distribute one batch of received packets.
 *
 * Dequeues all packets from the default netdev's RX queue, hashes
 * each to a worker, and enqueues. Called internally by the
 * distributor loop; exposed for custom distributor implementations.
 */
void dispatch_rx(void);

/// Get the SPSC ring for a worker (by worker index, 0-based).
struct spsc_ring *dispatch_get_ring(int worker_index);

/// Get the SPSC ring assigned to a core (by core index).
struct spsc_ring *dispatch_get_core_ring(int core_index);

/// Copy current dispatch statistics into @p stats.
void dispatch_get_stats(struct dispatch_stats *stats);
/// @}

/// @name Flow hashing
/// @{

/**
 * @brief Compute a hash over the 5-tuple for flow distribution.
 *
 * A simple XOR-and-mix hash. All parameters are in their raw byte
 * order (network or host).
 *
 * @return 32-bit hash value.
 */
static inline uint32_t flow_hash_5tuple(uint32_t src_ip, uint32_t dst_ip,
                                        uint16_t src_port, uint16_t dst_port,
                                        uint8_t protocol)
{
    uint32_t hash = src_ip ^ dst_ip;
    hash ^= ((uint32_t)src_port << 16) | (uint32_t)dst_port;
    hash ^= (uint32_t)protocol;
    // Spread the entropy across all bits
    hash ^= hash >> 16;
    hash ^= hash >> 8;
    return hash;
}
/// @}

#endif // EMMK_DISPATCH_H

