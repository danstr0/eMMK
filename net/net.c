/**
 * @file net.c
 * @brief Stack-wide services: per-core counters and housekeeping.
 */

#include "net.h"
#include "arp.h"
#include "timer.h"
#include "serial.h"
#include "string.h"

// ── Counters ───────────────────────────────────────────────────────────

struct net_core_stats net_core_stats[SMP_MAX_CORES];

/// Printable names, in @c enum net_stat order.
static const char *const net_stat_names[NET_STAT_COUNT] =
{
#define NET_STAT_NAME(id, name) [NET_STAT_##id] = name,
    NET_STAT_LIST(NET_STAT_NAME)
#undef NET_STAT_NAME
};

uint64_t net_stat_total(enum net_stat s)
{
    if ((unsigned)s >= NET_STAT_COUNT)
        return 0;

    uint64_t n = 0;
    for (int i = 0; i < SMP_MAX_CORES; ++i)
        n += __atomic_load_n(&net_core_stats[i].c[s], __ATOMIC_RELAXED);
    return n;
}

const char *net_stat_name(enum net_stat s)
{
    return (unsigned)s < NET_STAT_COUNT ? net_stat_names[s] : "?";
}

void net_stats_dump(void)
{
    kprintf("[NET] Counters (non-zero):\n");

    bool any = false;
    for (int s = 0; s < NET_STAT_COUNT; ++s)
    {
        uint64_t total = net_stat_total((enum net_stat)s);
        if (total == 0)
            continue;
        any = true;

        kprintf("  %-26s %lu", net_stat_names[s], total);

        // Per-core split, when more than one core counted it
        int cores = 0;
        for (int i = 0; i < SMP_MAX_CORES; ++i)
            if (net_core_stats[i].c[s] != 0)
                cores++;
        if (cores > 1)
        {
            kprintf("  (");
            for (int i = 0; i < SMP_MAX_CORES; ++i)
            {
                uint64_t v = net_core_stats[i].c[s];
                if (v != 0)
                    kprintf(" c%d=%lu", i, v);
            }
            kprintf(" )");
        }
        kprintf("\n");
    }

    if (!any)
        kprintf("  (none)\n");
}

void net_stats_reset(void)
{
    memset(net_core_stats, 0, sizeof(net_core_stats));
}

// ── Housekeeping ───────────────────────────────────────────────────────

/// Time of the next housekeeping pass (ms). Control core only.
static uint64_t net_next_housekeeping;

void net_housekeeping(void)
{
    uint64_t now = timer_get_ms();
    if (now < net_next_housekeeping)
        return;
    net_next_housekeeping = now + NET_HOUSEKEEPING_MS;

    arp_timer(now);
}

bool net_timers_pending(void)
{
    return arp_timers_pending();
}

