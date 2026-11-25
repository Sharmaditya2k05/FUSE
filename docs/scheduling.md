# Scheduling policies

Every block that SchedFS reads or writes becomes a request in one bounded queue
(capacity 128, configurable with `--queue=`). A single dispatcher thread repeatedly asks
`scheduler_pick()` which pending request to serve, serves it with `pread`/`pwrite`, and
wakes the thread that submitted it. The policy is chosen at mount time with `--sched=`.

```
FUSE thread ─ submit ─▶ [ bounded queue ] ─ scheduler_pick() ─▶ dispatcher ─▶ disk.img
     ▲                                                              │
     └──────────────── per-request "done" condition variable ◀──────┘
```

All policies break ties the same way: earlier arrival first, then lower request id. The
current time is an argument of `scheduler_pick()`, so tests and the simulator drive it
with a simulated clock and get exactly repeatable results.

## The six policies

| Policy | Rule | Movement charged |
|---|---|---|
| `fcfs` | oldest request | distance between served blocks |
| `sstf` | nearest block to the head | distance between served blocks |
| `scan` | nearest block in the current direction; if none is left, reverse | travel **to the disk edge** and back when it reverses |
| `look` | same choice as SCAN | distance between served blocks (reverses at the last request) |
| `adaptive` | lowest `α·distance/N − β·wait/max_wait` (heuristic baseline) | distance between served blocks |
| `deadline` | overdue requests first, earliest deadline first; otherwise nearest block | distance between served blocks |

FCFS, SSTF, SCAN and LOOK are the textbook algorithms. Deadline scheduling is an
established approach (the Linux "deadline" I/O scheduler gives reads and writes separate
expiry times in the same way). SchedFS's contribution is the integration and the
evaluation: these policies are implemented in a working file system's request path,
measured on real mounted traffic, and compared with the same instrumentation.

### Movement model (finding A)

"Movement" is a model: the number of blocks between two served requests, as if a disk
head moved over block numbers. It is defined once, in `scheduler_advance()`, and used by
the dispatcher (mounted metrics and the trace), the simulator (`schedfs-bench`) and the
tests. Previously the mounted dispatcher charged only the distance between served blocks
while the unit test charged SCAN for travel to the edge, so the same textbook trace gave
208 blocks mounted and 236 in the test. SCAN and LOOK are now separate policies:

| Textbook trace (head 53, moving toward 0) | Order | Movement |
|---|---|---|
| SCAN | 37, 14, (edge 0), 65, 67, 98, 122, 124, 183 | 53 + 183 = **236** |
| LOOK | 37, 14, 65, 67, 98, 122, 124, 183 | 39 + 169 = **208** |

`tests/test_regress.c` checks 236 (SCAN), 208 (LOOK) and 640 (FCFS) through the real
dispatcher, with the requests queued before the dispatcher starts.

## The deadline policy

Each request gets a **service-start deadline**:

```
deadline = arrival + read_deadline     (reads,  --read-deadline-ms,  default 0 ms)
deadline = arrival + write_deadline    (writes, --write-deadline-ms, default 2 ms)
```

At each decision:

1. If any pending request is overdue (`deadline <= now`), serve the one with the
   **earliest deadline**; ties by arrival, then id.
2. Otherwise serve the **nearest block** to the head (SSTF), ties by arrival, then id.

With both deadlines at 0 it is FCFS; with very long deadlines it is SSTF. The defaults
were chosen on separate tuning runs (seed 101, never used in the evaluation; see
[evaluation.md](evaluation.md)): first the read deadline (0 ms gave the lowest foreground
tail latency, `results/mounted/tuning.png`), then the write deadline (2 ms, because with
50 ms foreground *updates*, whose block writes queued behind nearby background writes,
had a worse tail than under FCFS; `results/mounted/tuning_write.png`).

What the defaults mean in practice: every read is overdue as soon as it arrives, so
reads are served oldest first; a write that has waited less than 2 ms can be passed by
reads and by nearer writes; once it has waited 2 ms it is served in deadline order. So
locality is used only within a short window, and no write waits long behind locality.

Classification is by operation type only (read or write of a block), which the request
already carries. There is no filename rule and no hidden per-process priority. The
caller's process id is recorded in the trace for measurement only.

### What is guaranteed, and when

* **Within one operation type** every deadline is `arrival + the same constant`, so a
  later arrival of that type can never overtake an overdue request of that type
  (tested: `test_deadline_order`, `test_starvation`).
* **Across types**, a read can overtake an overdue write only if the read's deadline is
  earlier, i.e. the read arrived before `write_deadline_time − read_deadline`. Reads
  arriving after that cannot overtake it, so a write is not starved by a continuous
  stream of reads (tested with a 50 ms write deadline: a distant write amid continuous
  nearby reads starts at 50.05 ms).
* **Bound on the start time.** Once a request R is overdue, the requests served before it
  are (a) the one already being served, because the dispatcher is non-preemptive, and (b)
  pending requests with earlier deadlines. With the bounded queue there are at most
  `Q − 1` of those at any moment, plus requests from producers blocked at admission. So R
  starts no later than `deadline(R) + (Q − 1 + P) · S_max + S_current`, where `S_max` is
  the longest service time and `P` the number of producers waiting for space.
* **This is not a real-time guarantee.** A deadline marks when a request becomes urgent,
  not when it completes. Under overload (more work than the disk can serve), every
  request becomes overdue and the policy degrades to earliest-deadline-first, i.e. FCFS
  within each type, with queues and waits growing as for any policy.
* **Operations are chains of requests.** A file operation issues several block requests
  one after another (about 11 for an open + 4 KiB read + close in the experiments), so
  its latency is a sum of request waits. That is why a small read deadline matters more
  than a large one (see `results/mounted/tuning.png`).

## Adaptive (finding C)

```
score = α · distance / N − β · wait / max_wait        (α = 0.7, β = 0.3; lowest wins)
```

Adaptive is kept as a documented **heuristic baseline** without a starvation guarantee.
The age term is normalized by the oldest pending wait, so it can never be worth more than
`β`, however long a request has waited. A request whose distance term exceeds that of
nearby work by more than `β` is therefore never chosen while nearby work keeps arriving:
with the defaults, a request more than `(β/α)·N ≈ 0.43·N` blocks farther than the
nearby work starves. The review's check is reproduced in `test_adaptive_bound`: an old
request 3,000 blocks away (N = 4,096) is chosen 0 times in 10,000 decisions with fresh
nearby work; a request 1,200 blocks away is chosen at once (`test_starvation`).
