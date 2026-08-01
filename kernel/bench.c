/**
 * @file bench.c
 * @brief Per-core benchmarking counters and periodic reporting.
 */

#include "bench.h"
#include "tsc.h"
#include "serial.h"
#include "string.h"

// ── Per-core counters (cache-line aligned) ─────────────────────────────

static struct bench_counters counters[SMP_MAX_CORES];

// ── Initialization ─────────────────────────────────────────────────────

void bench_init(void)
{
    uint64_t now = tsc_read();

    for (int i = 0; i < SMP_MAX_CORES; ++i)
    {
        memset(&counters[i], 0, sizeof(counters[i]));
        counters[i].cycles_min     = ~0UL;
        counters[i].interval_start = now;
    }
}

// ── Recording ──────────────────────────────────────────────────────────

void bench_record(int core, uint64_t cycles)
{
    struct bench_counters *c = &counters[core];

    c->packets++;
    c->interval_packets++;
    c->cycles_total += cycles;

    if (cycles < c->cycles_min)
        c->cycles_min = cycles;
    if (cycles > c->cycles_max)
        c->cycles_max = cycles;
}

// ── Reporting ──────────────────────────────────────────────────────────

void bench_report(void)
{
    uint64_t now = tsc_read();
    int cores = smp_get_core_count();

    uint64_t total_pps     = 0;
    uint64_t total_packets = 0;

    kprintf("\n--- bench report ---\n");

    for (int i = 0; i < cores; ++i)
    {
        struct bench_counters *c = &counters[i];

        if (c->packets == 0 && c->interval_packets == 0)
            continue;

        // Calculate packets per second for this interval
        uint64_t elapsed_us = tsc_elapsed_us(c->interval_start, now);
        uint64_t pps = 0;

        if (elapsed_us > 0 && c->interval_packets > 0)
            pps = c->interval_packets * 1000000 / elapsed_us;

        // Latency stats (only if packets were processed)
        uint64_t avg_ns = 0;
        uint64_t min_ns = 0;
        uint64_t max_ns = 0;

        if (c->packets > 0)
        {
            avg_ns = tsc_cycles_to_ns(c->cycles_total / c->packets);
            min_ns = (c->cycles_min != ~0UL)
                   ? tsc_cycles_to_ns(c->cycles_min)
                   : 0;
            max_ns = tsc_cycles_to_ns(c->cycles_max);
        }

        kprintf("  core %d: %lu pps, %lu total",
                i, pps, c->packets);
        kprintf("  latency (ns) min=%lu, avg=%lu, max=%lu\n",
                min_ns, avg_ns, max_ns);

        total_pps     += pps;
        total_packets += c->packets;

        // Reset interval counters
        c->interval_packets = 0;
        c->interval_start   = now;
    }

    if (total_packets > 0)
        kprintf("  aggregate: %lu pps, %lu total\n",
                total_pps, total_packets);
    else
        kprintf("  (no packets)\n");

    kprintf("--------------------\n");
}

