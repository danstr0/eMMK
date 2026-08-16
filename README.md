# eMMK

A from-scratch x86-64 kernel targeting QEMU, built as a foundation for low-latency, run-to-completion packet processing.

## What it does

Boots on QEMU via GRUB (or any Multiboot2-compliant loader), initializes a multi-core x86-64 system, drives a PCI virtio-net device, and runs a complete IPv4 network stack.

## Design philosophy

eMMK is not a general-purpose OS, but a *substrate*. It is the minimum viable kernel for running low-latency, run-to-completion network applications directly on hardware.

The design borrows from DPDK: no preemptive scheduler, no system calls, no user/kernel split — all code runs in ring 0. Each core runs a tight poll loop, processing packets to completion without blocking. Interrupts are used only for bootstrapping (timer calibration, AP startup) — the data path is entirely polling-driven.

This means the developer controls everything: which cores do what, how packets are processed, when memory is allocated, etc. The kernel provides the mechanisms without imposing policy. This kind of philosophy trades generality for determinism — the same tradeoff real ultra-low-latency packet-processing frameworks make.

## Building and running

**Prerequisites:** `gcc`, `nasm`, `ld`, `grub-mkrescue` (+ `xorriso`), `qemu-system-x86_64`

```
make test   # Run all test suites
make run    # Boot with 4 cores; ping gateway
make demo   # Run kernel demo (see below)
make clean
```

## Subsystems

**Boot:** Multiboot2 entry in 32-bit protected mode, transitions through paging setup and a GDT load into 64-bit long mode, then jumps to the higher-half kernel. AP cores follow a separate path via a real-mode trampoline copied to physical `0x8000`.

**Memory management:** Bitmap-based physical page allocator (PMM), 4-level page table walker (VMM), first-fit heap, and a slab allocator with per-core magazine caching for lock-free fast-path allocation.

**Interrupts:** Full IDT with per-exception handlers, legacy PIC (disabled), LAPIC + IOAPIC with MSI-X support, IST stacks for double fault and NMI.

**SMP:** BSP discovers cores via CPUID and boots each AP via the INIT-SIPI-SIPI sequence. Each AP gets its own stack, TSS (with IST entries), and a GS-based per-CPU data area. Cores can be assigned custom roles via `smp_assign_role()` and launched into their work functions.

**Synchronization:** Ticket spinlock with FIFO ordering and interrupt-safe (`irqsave`/`irqrestore`) variants. The PMM, heap, and slab allocators are lock-protected, and per-core slab magazines provide a lockless allocation fast path.

**PCI and virtio-net:** PCI bus enumeration with BAR decoding, MSI-X capability parsing, and a virtio (legacy) network driver in polling mode.

**Network stack:** `netbuf` (modeled after Linux's `sk_buff`) provides zero-copy buffer management. The stack processes Ethernet → ARP/IPv4 → ICMP/UDP. ARP includes a 64-entry cache with timeout and pending-packet queuing; UDP provides a callback-based bind table for port demuxing.

**Instrumentation:** TSC-calibrated cycle counter for nanosecond-resolution timing; per-core log levels with core-tagged output; serial spinlock for safe multi-core printing.

## Testing

The test infrastructure uses a compile-time flag (`-DKERNEL_TEST`) that replaces the normal boot path with a test runner. QEMU's `isa-debug-exit` device signals pass/fail to the host.

```
==== eMMK test runner ====
[suite] types
  result: 41 passed, 0 failed
[suite] string
  result: 46 passed, 0 failed
[suite] spinlock
  result: 38 passed, 0 failed
[suite] pmm
  result: 21 passed, 0 failed
[suite] vmm
  result: 22 passed, 0 failed
[suite] heap
  result: 23 passed, 0 failed
[suite] slab
  result: 25 passed, 0 failed
[suite] interrupts
  result: 8 passed, 0 failed
[suite] smp
  result: 26 passed, 0 failed
[suite] pci_net
  result: 16 passed, 0 failed
[suite] netbuf
  result: 52 passed, 0 failed
[suite] net
  result: 34 passed, 0 failed

==========================
all tests passed
```

## Demo: UDP echo server with software RSS

`make demo` boots the kernel in multi-core (4 cores) distributed mode with a UDP echo server on port 7.

The demo configures the system as a pipeline: core 0 runs as the *distributor* (polls virtio-net, hashes each packet's 5-tuple, enqueues on worker rings), and cores 1-3 run as *workers* (dequeue from their lock-free SPSC rings and process packets through the full stack). ARP is handled directly by the distributor to keep the ARP cache single-threaded.

```
[DEMO] UDP echo server on port 7
[DEMO] 3 worker(s), distributor on core 0
[DEMO] Reporting every 5 seconds
[DEMO] Test with: echo hello | nc -u 127.0.0.1 8080

[SMP] Launching all cores...
[DEMO] Distributor running on core 0
[DEMO] Worker running on core 1
[DEMO] Worker running on core 2
[DEMO] Worker running on core 3
```

Each worker wraps packet processing with TSC timestamps, feeding per-core latency and throughput counters. The distributor prints a benchmark report every 5 seconds showing per-core packets/sec and min/avg/max processing latency (in nanoseconds).

To send traffic to the echo server from the host (QEMU forwards from port 8080 to port 7):

```
echo "hello" | timeout 0.2 nc -u 127.0.0.1 8080
```

The echo server reflects the payload back unchanged, exercising the full network stack.

## Limitations and future work

- **Single virtio-net queue:** eMMK has no hardware RSS or multiqueue. The current software distributor with per-core SPSC rings exists as a 'baseline' to benchmark future (optimized) versions of the kernel against. In the future, a "share-nothing" model — in which each core owns an independent virtio-net queue and runs its own full stack instance — will be implemented.
- **1 GiB physical memory:** The boot page tables identity-map the first 1 GiB with 2 MiB huge pages. Extending this to full physical memory requires building a complete linear map of all RAM in `vmm_init()`.
- **16-core SMP cap:** `SMP_MAX_CORES` is a compile-time constant, easily raised (though the runtime GDT and percpu allocation scale linearly).
- **QEMU-only:** eMMK assumes sequential APIC IDs. Real hardware would require ACPI MADT parsing for core discovery
- **Latency histogram:** The benchmarking module will be extended with fixed-bucket histograms for P50/P95/P99 reporting.

## Documentation

Visit [danstr0.github.io/eMMK/](https://danstr0.github.io/eMMK/)

