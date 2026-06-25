# Kernel & System Tuning for Low Latency

Fast code on a noisy machine is still slow at the tail. This guide covers the
Linux-level controls that quiet the machine so the latency-sensitive threads run
without interference. Each section explains **what** to change and, more
importantly, **why it matters for latency**.

> These settings trade throughput, power efficiency, and general-purpose
> fairness for *determinism*. Apply them only to the cores dedicated to the hot
> path, and benchmark before/after — never cargo-cult.

---

## 1. Core isolation: `isolcpus`, `nohz_full`, `rcu_nocbs`

Set on the kernel command line (e.g. in GRUB `GRUB_CMDLINE_LINUX`):

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

- **`isolcpus=2,3`** removes cores 2 and 3 from the kernel scheduler's general
  load balancing. Ordinary tasks will not be scheduled there, so your pinned
  thread is not preempted by random background work. *Why:* a single preemption
  can cost microseconds (context switch + cold caches) — fatal for a p99.9
  target.
- **`nohz_full=2,3`** enables full dynticks on those cores: when exactly one
  runnable task is present, the kernel stops the periodic scheduler tick (the
  1000 Hz / 250 Hz timer interrupt). *Why:* the tick is a recurring, periodic
  interrupt that flushes pipeline state and pollutes caches every few
  milliseconds — exactly the kind of periodic jitter that shows up at the tail.
- **`rcu_nocbs=2,3`** offloads RCU (read-copy-update) callback processing off the
  isolated cores onto housekeeping cores. *Why:* RCU callbacks otherwise run in
  softirq context on the local core and can stall your thread unpredictably.

Pin the hot threads to these cores with `llt::sys::pin_this_thread()`.

---

## 2. IRQ affinity

Steer device interrupts **away** from the isolated cores:

```bash
# Stop irqbalance from moving IRQs back onto your isolated cores.
systemctl stop irqbalance

# Route an IRQ (e.g. 129) to housekeeping cores 0-1 (mask 0x3).
echo 3 > /proc/irq/129/smp_affinity
```

*Why:* an interrupt handler that fires on your hot core preempts the trading
thread, runs in interrupt context with caches cold to your data, and adds
unbounded jitter. Keeping all IRQs on housekeeping cores leaves the hot cores
exclusively for your code.

---

## 3. CPU frequency governor = `performance`

```bash
for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$c"
done
```

*Why:* the default `powersave`/`ondemand`/`schedutil` governors scale frequency
up only *after* they detect load. That ramp-up means the first operations after
an idle period run at a low clock and are slow — a classic source of latency
spikes on bursty workloads. `performance` pins the cores at their top frequency
so there is no ramp-up latency. (Also disable Intel Turbo variability if you need
*consistent* — not just high — clocks.)

---

## 4. Disable deep C-states (idle states)

Either bound the exit latency at boot:

```
intel_idle.max_cstate=1 processor.max_cstate=1 idle=poll
```

…or, more surgically, prevent the hot cores from entering deep idle by keeping a
busy-poll loop (the library's `cpu_pause()` spin) running on them.

*Why:* when a core enters a deep C-state (C3/C6) it powers down caches and parts
of the core; waking it back up to handle the next message costs **microseconds**.
For a thread that must react instantly to a rare event, that wake-up latency is
the dominant tail contributor. `idle=poll` keeps the core spinning at the cost of
power and heat.

---

## 5. Huge pages

Reserve explicit huge pages:

```bash
echo 512 > /proc/sys/vm/nr_hugepages     # 512 * 2 MiB = 1 GiB
```

…then back hot allocations with them via `llt::sys::allocate_hugepages()`, or let
Transparent Huge Pages promote regions via `llt::sys::advise_hugepages()` (THP
mode set in `/sys/kernel/mm/transparent_hugepage/enabled`).

*Why:* the TLB caches virtual→physical translations. With 4 KiB pages, a few MiB
of hot data needs hundreds of TLB entries and you take TLB misses (each a
page-table walk of tens to hundreds of cycles) right in the critical path. A
2 MiB huge page covers 512× more memory per entry, so big hot structures (ring
buffers, pools, order books) fit in far fewer entries and miss far less.

---

## 6. NIC tuning: busy-poll / kernel bypass

For the network path:

- **Busy-poll** (`SO_BUSY_POLL` socket option, `net.core.busy_poll` /
  `busy_read` sysctls): the socket read spins polling the NIC queue instead of
  sleeping and waiting for an interrupt. *Why:* it trades CPU for the elimination
  of interrupt + softirq + wakeup latency on packet receive.
- **Kernel bypass** (DPDK, Solarflare Onload, AF_XDP): move the NIC datapath into
  userspace entirely. *Why:* this removes the kernel networking stack, interrupts,
  and copies from the hot path — the largest single latency win for market-data
  ingestion, at the cost of significant complexity.
- **Disable interrupt coalescing** (`ethtool -C ethX rx-usecs 0`) when you want
  the lowest per-packet latency rather than the best throughput.

---

## 7. Other quieting measures

- **Disable Transparent Huge Page *defragmentation* stalls** if using THP:
  set `defrag` to `defer` or `madvise` so a hot thread is not blocked compacting
  memory.
- **Lock memory** (`mlockall(MCL_CURRENT | MCL_FUTURE)`) to prevent your pages
  from being swapped — a major-fault on the hot path is catastrophic.
- **`taskset`/cgroup `cpuset`** to confine *everything else* to the housekeeping
  cores, complementing `isolcpus`.
- **Disable SMT (hyper-threading)** on hot cores if a sibling thread would
  contend for shared execution units, or dedicate the sibling and leave it idle.
- **Pre-fault and warm up**: touch all pooled memory and run the hot path a few
  thousand times before going live so caches, TLB, and branch predictors are
  warm and the first real event is not the slow one.

---

## Verifying

- `cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor` → `performance`.
- `cat /proc/irq/*/smp_affinity` → no isolated cores in the masks.
- `cyclictest -p99 -t -a2,3` (rt-tests) → low, tight max latency on the isolated
  cores.
- Re-run `examples/example_spsc` pinned to two isolated cores and compare
  p99.9 before/after each change. Measure; do not assume.
