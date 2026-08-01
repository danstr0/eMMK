/**
 * @file bench.h
 * @brief Per-core benchmarking counters and reporting.
 *
 * Provides zero-lock, per-core packet and latency counters for
 * measuring engine throughput and per-packet processing time.
 *
 * @par Design
 * Each core writes only to its own @c bench_counters struct (indexed
 * by core number), so no synchronization is needed on the recording
 * path. The reporting function reads all counters from a single core
 * (the distributor).
 *
 * @par Usage
 * @code{.c}
 *   // In the worker loop, around packet processing:
 *   uint64_t t0 = tsc_read();
 *   eth_input(nb);
 *   uint64_t t1 = tsc_read();
 *   bench_record(core_index, t1 - t0);
 *
 *   // Periodically from the distributor:
 *   bench_report();
 * @endcode
 */

#ifndef EMMK_BENCH_H
#define EMMK_BENCH_H

#include "types.h"
#include "smp.h"

/// @name Per-core counters
/// @{

/**
 * @brief Per-core benchmark counters.
 *
 * Cache-line aligned so each core's counters occupy a distinct cache
 * line — no false sharing between cores.
 */
struct bench_counters
{
    uint64_t packets;           ///< Total packets processed.
    uint64_t cycles_total;      ///< Sum of per-packet TSC cycles.
    uint64_t cycles_min;        ///< Minimum per-packet cycles.
    uint64_t cycles_max;        ///< Maximum per-packet cycles.

    // Interval tracking
    uint64_t interval_packets;  ///< Packets since last report.
    uint64_t interval_start;    ///< TSC at start of current interval.
} ALIGNED(64);
/// @}

/// @name Initialization
/// @{

/// Reset all counters and start the first measurement interval.
void bench_init(void);
/// @}

/// @name Recording
/// @{

/**
 * @brief Record a processed packet.
 *
 * Call after processing a packet with the TSC delta (processing
 * cycles). Only the owning core writes to its counters.
 *
 * @param core    Core index.
 * @param cycles  TSC cycles spent processing this packet.
 */
void bench_record(int core, uint64_t cycles);
/// @}

/// @name Reporting
/// @{

/**
 * @brief Print a summary of all cores' performance.
 *
 * Shows per-core and aggregate packets/sec, plus min/avg/max
 * latency in nanoseconds. Resets the interval counters for the
 * next reporting period.
 *
 * Safe to call from any core.
 */
void bench_report(void);
/// @}

#endif // EMMK_BENCH_H

