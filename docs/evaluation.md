# Evaluation

**Question.** While background workers write large files, does the deadline policy keep
small foreground file operations responsive, compared with SchedFS's other policies, and
what does it cost the background work?

## Four kinds of evidence (do not mix them up)

| Kind | Tool | What it shows | What it does not show |
|---|---|---|---|
| Deterministic scheduler simulation | `schedfs-bench` (`src/sched/sim.c`) | exact order, modeled movement and simulated waits for given arrivals; repeatable | anything about the file system, FUSE, caching or a real disk |
| Scheduler and core unit/regression tests | `make unit`, `make tsan` | correctness of ordering, deadlines, locking, truncate, persistence | performance |
| Mounted file-system experiments | `scripts/experiments.py` | measured end-to-end latency of real file operations through FUSE, background throughput, queue behaviour | physical-disk behaviour (the disk is virtual), other machines |
| Physical storage / application performance | not done | | |

"Modeled movement" (block distance) is reported separately and is never presented as a
speed-up. A lower movement total in the simulator does not imply equally faster I/O.

## Mounted experiment setup

* **Environment**: Docker Desktop 29.x on Windows 11, WSL 2 kernel
  6.18.58-microsoft-standard-WSL2, 20 logical CPUs, container `ubuntu:24.04`
  (`docker/Dockerfile`), libfuse 3.14.0, gcc 13.3, matplotlib 3.6, run as an
  unprivileged user. Each run
  records the kernel version, CPU count, date and git commit in `run.json`.
* **Image**: a fresh 64 MiB image (16,384 blocks) for every run, created by
  `mkfs.schedfs` in a temporary directory and deleted afterwards
  (`scripts/run_mounted.sh`).
* **Two storage configurations**:
  * `direct`: `--direct-io`, i.e. the image is opened with `O_DIRECT`, so block requests
    bypass the host page cache and reach the virtual disk of the WSL 2 VM (an SSD on the
    host). No modeled delay. These are storage-reaching measurements on this machine.
  * `model`: `--direct-io --model-base-us=100 --model-us-per-block=0.25`. Same I/O, but
    the dispatcher holds each request for at least `100 µs + 0.25 µs × blocks moved`
    (a full stroke of 16,384 blocks ≈ 4.2 ms). This MODELS a disk where position matters;
    latencies are measured, but on a modeled device.
* **Caching**: on the client side, SchedFS mounts with libfuse defaults (no `kernel_cache`,
  no writeback cache), and the foreground opens the file for every operation, so each
  read reaches the daemon; the trace confirms it (about 11 block requests per foreground
  operation). On the image side, `O_DIRECT` avoids the host page cache. System-wide caches
  are not dropped (no privileged operations were used).
* **Workload** (`schedfs-workload`, identical options for every policy): one foreground
  process opens `fg.dat` (64 KiB), reads or updates 4 KiB at a seeded random offset and
  closes it, every 20 ms; N background processes each write 2 MiB files in 128 KiB
  writes, again and again (`O_TRUNC` each round, so blocks are freed and allocated
  again). The copy mode reads a 2 MiB source file and writes a copy. 1 s warm-up, then
  10 s measured; 3 repetitions with seeds 1001, 1002, 1003 (about 500 foreground
  samples per run, 1,500 per cell).
* **Workloads**: `bg2_read` (2 writers + small reads), `bg2_mix` (reads and updates),
  `bg4_read` (4 writers), `copy2_read` (2 copiers: background reads too), `bg2_update`
  (foreground writes), `light` (no background: overhead).
* **Metrics**: foreground latency p50/p95/p99 (per run, then mean and min..max over runs;
  and pooled over all samples), background MB/s (bytes of rounds completed in the measured
  window), block-level queue wait for foreground and background requests (from
  `--trace`), maximum observed wait, request counts, queue depth, modeled movement.
* **Equivalence of traces**: every policy gets the same workload specification, seed and
  a fresh image, but the block request sequences are not identical: concurrency and
  allocation timing change which blocks are requested when. The trace of each run records
  the actual sequence; `trace.csv.gz` is kept in the repository for the first repetition
  of `bg2_read` under FCFS and Deadline in both configurations (the others were summarized
  into each run's `summary.json` and then deleted to keep the repository small).
* **Host reference**: the same workload on the container's own Linux file system
  (overlayfs on ext4 in the WSL 2 VM, page cache on, no FUSE, no scheduler). It shows how
  far FUSE plus SchedFS's design is from native; it is not a fair comparison of
  scheduling policies (different architecture, caching and functionality).

## Criteria and tuning (fixed before the evaluation runs)

Primary: foreground p95 and p99 latency (lower is better) and background MB/s (higher is
better). Secondary: block-level foreground wait, movement, queue depth. The rule for
choosing a parameter: the lowest mean of (p99 / FCFS p99) over the tuning cells, with
background throughput not clearly below FCFS. All tuning used seed 101 and 2 repetitions
of 8 s; the evaluation seeds (1001-1003) were not used.

1. **Read deadline** (`bg2_read`, write deadline 50 ms, read deadline 0-5 ms;
   `results/mounted/tuning/`, `tuning.png`). p99 / FCFS p99, mean of direct and model:
   0 ms 0.69 (0.67, 0.71), 0.1 ms 0.80, 0.25 ms 0.99, 0.5 ms 1.33, 1 ms 1.59, 2 ms 1.79,
   5 ms 1.81. Background throughput at 0 ms was 1.11 and 1.12 times FCFS. 0 ms was chosen.
2. **Write deadline.** An earlier full evaluation run with a 50 ms write deadline (kept
   outside the repository; its throughput used the coarser round-level measurement) showed
   that foreground *updates* had a much worse tail than under FCFS: their block writes
   queued behind nearby background writes. Writes were then tuned on `bg2_read` and
   `bg2_update` (read deadline 0, write deadline 1-50 ms; `results/mounted/tuning_write/`,
   `tuning_write.png`):

   | write deadline | p99 / FCFS: direct read, direct update, model read, model update | mean | background MB/s / FCFS (same cells) |
   |---|---|---|---|
   | 1 ms | 0.70, 1.06, 0.73, 0.82 | 0.83 | 1.22, **0.78**, 1.08, 1.11 |
   | **2 ms** | **0.63, 1.26, 0.67, 0.82** | **0.85** | **1.38, 0.92, 1.12, 1.15** |
   | 5 ms | 0.89, 2.11, 0.63, 1.06 | 1.17 | 1.04, 0.84, 1.12, 1.22 |
   | 10 ms | 0.77, 2.30, 0.69, 1.38 | 1.28 | 1.01, 0.90, 1.10, 1.30 |
   | 50 ms | 12.58, 1.90, 0.68, 1.72 | 4.22 | 0.92, 1.01, 1.14, 1.29 |

   1 ms had the lowest mean but cost 22% of background throughput on the direct update
   cell, outside that cell's run-to-run variation (FCFS itself measured 19.5 and 22.8
   MB/s there); 2 ms was chosen. (The 12.58 is one run with a 145 ms outlier; without it
   50 ms is still the worst on updates.) Even at 2 ms, foreground updates in the direct
   configuration remain slower than under FCFS at p99.

The evaluation below uses the defaults `--read-deadline-ms=0 --write-deadline-ms=2`.

## Reproduce

```sh
docker build -t schedfs-dev docker
docker run --rm --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined \
  -v "$PWD:/src" -w /src schedfs-dev bash -c "make && chmod -R a+rwX results && su student -c '
    python3 scripts/experiments.py tune   results/mounted/tuning &&
    python3 scripts/experiments.py matrix results/mounted/matrix --reps 3 --duration 10 --keep-first-trace &&
    python3 scripts/experiments.py host   results/mounted --reps 3 --duration 10 &&
    python3 scripts/report_results.py     results/mounted'"
```

In `results/mounted/` the per-run raw files are stored gzipped (`fg.csv.gz`,
`bg_*.csv.gz`, and `trace.csv.gz` where kept) next to `summary.json`, `roles.csv`,
`workload.json` and `run.json`; `analyze_run.py` and `report_results.py` read either form.

The matrix takes about 50 minutes (216 mounted runs). A single run:
`scripts/run_mounted.sh OUT "--sched=deadline --direct-io" "--duration 10 --bg 2"`, then
`python3 scripts/analyze_run.py OUT`.

## Results

All numbers: mean of 3 runs (seeds 1001-1003), 10 s each after a 1 s warm-up; brackets
give the range over the 3 runs. Full tables with every policy, pooled percentiles and
block-level columns: [`results/mounted/summary.md`](../results/mounted/summary.md)
(machine-readable: `summary.csv`; raw data per run under `results/mounted/matrix/`).

### Foreground p99 latency (ms, lower is better) and background throughput (MB/s)

**direct** (real I/O with `O_DIRECT`, no modeled delay):

| Workload | FCFS | SSTF | LOOK | Adaptive | **Deadline** | Deadline vs FCFS | Deadline vs SSTF |
|---|---|---|---|---|---|---|---|
| 2 writers + reads | 4.98 · 21.8 MB/s | 11.29 · 22.4 | 7.41 · 19.5 | 5.20 · 20.0 | **3.89 (3.57-4.42) · 19.1 (16.7-21.3)** | p99 −22%, MB/s −13% | p99 −66%, MB/s −15% |
| 4 writers + reads | 8.73 · 18.6 | 19.95 · 18.8 | 9.96 · 18.8 | 8.17 · 17.7 | **4.22 (3.88-4.59) · 17.8 (16.8-18.7)** | −52%, −4% | −79%, −5% |
| 2 copiers + reads | 10.64 · 9.5 | 25.14 · 9.9 | 15.51 · 8.8 | 11.91 · 8.0 | **7.42 (6.78-8.55) · 9.0 (8.7-9.5)** | −30%, −4% | −70%, −9% |
| 2 writers + reads/updates | 7.20 · 18.1 | 14.28 · 21.1 | 8.46 · 21.6 | 6.50 · 20.5 | **7.11 (6.69-7.40) · 19.2 (17.4-21.9)** | −1%, +6% | −50%, −9% |
| 2 writers + updates | 5.83 · 19.5 | 13.74 · 20.4 | 7.97 · 18.4 | 6.88 · 18.4 | **7.22 (6.63-7.55) · 20.7 (17.3-23.4)** | **+24%**, +6% | −47%, +1% |
| no background | 2.27 | 2.52 | 2.79 | 2.75 | **2.78 (2.04-3.84)** | within run-to-run range | |

**model** (the same I/O plus a modeled seek time of 100 µs + 0.25 µs per block moved):

| Workload | FCFS | SSTF | LOOK | Adaptive | **Deadline** | Deadline vs FCFS | Deadline vs SSTF |
|---|---|---|---|---|---|---|---|
| 2 writers + reads | 20.64 · 3.40 MB/s | 33.85 · 4.56 | 21.98 · 3.65 | 21.03 · 3.45 | **15.14 (14.49-15.76) · 3.66 (3.60-3.74)** | p99 −27%, MB/s +8% | p99 −55%, MB/s −20% |
| 4 writers + reads | 70.20 · 1.18 | 44.01 · 3.02 | **28.80 · 1.91** | 59.74 · 1.32 | 30.76 (28.86-31.94) · 1.22 (1.21-1.23) | −56%, +3% | −30%, −60% |
| 2 copiers + reads | 40.34 · 0.89 | 45.88 · 1.90 | 39.46 · 1.36 | 44.85 · 0.90 | **26.93 (24.29-28.70) · 0.81 (0.80-0.84)** | −33%, −9% | −41%, −57% |
| 2 writers + reads/updates | 25.20 · 3.06 | 41.46 · 4.66 | 26.25 · 3.53 | 24.14 · 3.13 | **18.90 (18.28-19.74) · 3.42 (3.36-3.46)** | −25%, +12% | −54%, −27% |
| 2 writers + updates | 22.73 · 3.10 | 40.74 · 4.61 | 25.70 · 3.58 | 22.55 · 3.16 | **19.05 (18.55-19.33) · 3.55 (3.51-3.62)** | −16%, +15% | −53%, −23% |
| no background | 3.65 | 3.49 | 3.64 | 3.53 | **3.64 (3.56-3.69)** | same | |

SCAN is left out of these two tables because it is an outlier: under the modeled seek
time every reversal pays the trip to the disk edge (16,384 blocks), so its p99 is
82-119 ms with background load and 36.7 ms even with none. That is the cost of the SCAN
definition now being charged consistently (finding A); LOOK is the practical variant.

**Host file system reference** (same workload on the container's overlayfs/ext4 with the
page cache, no FUSE): foreground p99 0.04-0.07 ms and background 600-2,800 MB/s. SchedFS
is about two orders of magnitude slower per operation, because every operation crosses
FUSE, and every block goes through one queue to `O_DIRECT` I/O with no cache. This is a
reference point, not a scheduling comparison.

### Does the workload exercise the scheduler?

Yes, with background load. In every background workload 93-100% of foreground block
requests found background requests already waiting when they arrived, the queue held
1.1-3.9 requests on average when the scheduler picked (up to 5), and one foreground
operation issued 11.5 (reads) to 15.6 (copy workload) block requests. With O_DIRECT,
81-93% (direct) and 94-99% (model) of a foreground operation's time was spent in the block layer (queue wait plus
service), so the order of service matters. Without background load the queue held one
request at every pick and the policies cannot differ, which is what the "no background"
rows show. (Earlier, without `O_DIRECT` and with one writer, only 0.1-0.2% of picks had
two or more requests pending: the host page cache made service so fast that requests
rarely overlapped. That is why the experiments use `O_DIRECT`.)

### What the results say

* **Deadline versus FCFS** (the fair baseline that is never starved): lower foreground
  p99 in 8 of 10 background cells, by 16-56%; about equal on mixed reads/updates in the
  direct configuration (−1%); **worse by 24% on foreground updates in the direct configuration**.
  Background throughput: −13% to +6% (direct, mostly inside the run-to-run ranges) and −9%
  to +15% (model).
* **Deadline versus SSTF** (pure locality): 30-79% lower foreground p99 in every cell. The
  cost is background throughput in the model configuration (−20% to −60%), where SSTF's
  locality pays off but starves foreground requests (SSTF's longest foreground queue wait
  was 10-55 ms; Deadline's 0.9-7.6 ms) and sometimes background ones (924 ms in
  `bg4_read`). In the direct configuration the throughput difference to SSTF is 1-15%.
* **Where Deadline is not best**: on 4 writers with the modeled seek time, LOOK had a
  slightly lower p99 (28.8 vs 30.8 ms) and 57% more throughput; on direct updates, FCFS
  and Adaptive had a lower p99.
* **Overhead**: with no background load all policies were within each other's run-to-run
  ranges (direct p50 1.44-1.61 ms; model p99 3.49-3.65 ms), so no scheduling overhead is
  measurable at this scale.
* **Block level versus application level**: Deadline cut the foreground requests' 99th
  percentile queue wait to 0.3-2.8 ms (SSTF 4.7-15.4 ms), and this carried through to the
  application p99 where foreground requests were reads. Where the foreground also writes
  (updates), its writes get the 2 ms write deadline and can wait behind other overdue
  writes, which is why direct updates did not improve.

### Answer to the evaluation question

On this machine, for small foreground **reads** during background writes or copies,
SchedFS's deadline policy lowered tail latency (p99) by 22-56% relative to SchedFS's FCFS
policy and by 30-79% relative to its SSTF policy, in both the direct and the modeled
configuration. The cost relative to FCFS was small and mostly within run-to-run
variation (−13% to +15% background throughput); relative to SSTF it gave up 20-60% of
background throughput where seek time is modeled. It did not help small foreground
**updates** on direct I/O (24% worse than FCFS at p99), and with four writers on the
modeled disk LOOK did slightly better. These are measurements of SchedFS on one Windows 11
+ WSL 2 machine, with a modeled seek time in one configuration; they make no claim about
physical hard disks or kernel file systems.
