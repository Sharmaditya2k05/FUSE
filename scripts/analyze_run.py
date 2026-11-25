#!/usr/bin/env python3
"""Summarize one mounted experiment directory written by scripts/run_mounted.sh.

Usage: python3 scripts/analyze_run.py OUTDIR [--json]

Application level (from the workload's own clock):
  foreground operation latency (open + read/update + close) after the warm-up,
  background bytes per second.
Block level (from schedfs --trace):
  queue wait (arrival to service start) for foreground and background requests,
  queue depth when each request was picked, how often a foreground request found
  background requests already waiting, and the modeled head movement.
Both clocks are CLOCK_MONOTONIC in the same kernel, so the two levels can be joined:
each foreground operation is matched with the block requests its process issued
while the operation was running.
"""
import bisect
import csv
import gzip
import json
import pathlib
import sys


def pct(values, q):
    """Nearest-rank percentile (q in 0..100); None for an empty list."""
    if not values:
        return None
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(-(-q * len(s) // 100)) - 1))
    return s[k]


def stats(values):
    if not values:
        return {"n": 0}
    return {"n": len(values), "mean": sum(values) / len(values), "p50": pct(values, 50),
            "p95": pct(values, 95), "p99": pct(values, 99), "max": max(values)}


def read_trace(path):
    with open(path) as f:
        header = f.readline()
        params = dict(kv.split("=", 1) for kv in header[1:].split())
        rows = list(csv.DictReader(f))
    return params, rows


def summarize(outdir):
    out = pathlib.Path(outdir)
    meta = json.loads((out / "workload.json").read_text())
    t0 = meta["t0_monotonic_ns"]
    warm = meta["warmup_s"] * 1e9
    window = (t0 + warm, t0 + (meta["warmup_s"] + meta["duration_s"]) * 1e9)
    roles = {}
    with open(out / "roles.csv") as f:
        for r in csv.DictReader(f):
            roles[int(r["pid"])] = r["role"]

    fg_ops = []
    fg_path = out / "fg.csv"
    with (open(fg_path) if fg_path.exists() else gzip.open(out / "fg.csv.gz", "rt")) as f:
        for r in csv.DictReader(f):
            start = int(r["start_ns"])
            if start >= warm:
                fg_ops.append((t0 + start, int(r["total_ns"]), r["op"], int(r["ok"])))
    fg_lat = [d / 1e6 for _, d, _, _ in fg_ops]

    bg_bytes, bg_time = 0, 0
    # background progress files may be stored gzipped in results/ to save space
    for p in sorted(list(out.glob("bg_*.csv")) + list(out.glob("bg_*.csv.gz"))):
        with (gzip.open(p, "rt") if p.suffix == ".gz" else open(p)) as f:
            rows = list(csv.DictReader(f))
        # bytes of the chunks completed inside the measured window
        done = [r for r in rows if warm <= int(r["end_ns"]) <= window[1] - t0]
        bg_bytes += sum(int(r["bytes"]) for r in done)
        result_errors = sum(int(r["errors"]) for r in rows)
        if result_errors:
            print(f"warning: {p}: {result_errors} background errors", file=sys.stderr)
    bg_time = (window[1] - window[0]) / 1e9

    result = {
        "dir": str(out),
        "workload": meta,
        "fg_latency_ms": stats(fg_lat),
        "fg_errors": sum(1 for o in fg_ops if not o[3]),
        "bg_mb_per_s": bg_bytes / bg_time / 1e6 if bg_time else None,
    }
    if (out / "run.json").exists():
        result["run"] = json.loads((out / "run.json").read_text())

    trace_path = out / "trace.csv"
    if trace_path.exists():
        params, rows = read_trace(trace_path)
        base = int(params["started_monotonic_ns"])
        result["schedfs"] = {k: v for k, v in params.items() if k != "started_monotonic_ns"}
        reqs = []
        for r in rows:
            a = base + int(r["arrival_ns"])
            if not (window[0] <= a <= window[1]):
                continue
            reqs.append((a, base + int(r["start_ns"]), base + int(r["end_ns"]),
                         roles.get(int(r["client"]), "other"), int(r["move"]), int(r["depth"]),
                         int(r["client"])))
        wait = {"fg": [], "bg": [], "other": []}
        for a, s, e, role, _, _, _ in reqs:
            wait[role].append((s - a) / 1e6)
        depths = [q[5] for q in reqs]
        # how many background requests were waiting when each foreground request arrived
        events = sorted([(a, 1) for a, s, e, role, *_ in reqs if role == "bg"] +
                        [(s, -1) for a, s, e, role, *_ in reqs if role == "bg"])
        times, level, running = [], [], 0
        for t, d in events:
            running += d
            times.append(t)
            level.append(running)
        fg_arrivals = [a for a, s, e, role, *_ in reqs if role == "fg"]
        competing = []
        for a in fg_arrivals:
            i = bisect.bisect_right(times, a) - 1
            competing.append(level[i] if i >= 0 else 0)
        # join foreground operations with the block requests they issued
        fg_pid = [p for p, r in roles.items() if r == "fg"]
        fg_reqs = sorted((a, s, e) for a, s, e, role, _, _, c in reqs if c in fg_pid)
        starts = [x[0] for x in fg_reqs]
        block_share, reqs_per_op = [], []
        for t, d, _, _ in fg_ops:
            lo, hi = bisect.bisect_left(starts, t), bisect.bisect_right(starts, t + d)
            inside = fg_reqs[lo:hi]
            reqs_per_op.append(len(inside))
            if d > 0:
                block_share.append(sum(e - a for a, s, e in inside) / d)
        dur = (window[1] - window[0]) / 1e9
        result["block"] = {
            "requests": len(reqs),
            "requests_per_s": len(reqs) / dur,
            "modeled_movement_blocks": sum(q[4] for q in reqs),
            "wait_ms": {k: stats(v) for k, v in wait.items() if v},
            "service_ms": stats([(e - s) / 1e6 for a, s, e, *_ in reqs]),
            "queue_depth": {"mean": sum(depths) / len(depths) if depths else None,
                            "max": max(depths) if depths else None,
                            "share_picked_with_2plus": (sum(1 for x in depths if x >= 2) /
                                                        len(depths)) if depths else None},
            "fg_requests_finding_bg_waiting": (sum(1 for c in competing if c > 0) /
                                               len(competing)) if competing else None,
            "fg_requests_per_op": stats(reqs_per_op),
            "fg_op_share_in_block_layer": stats(block_share),
        }
    return result


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    s = summarize(sys.argv[1])
    if "--json" in sys.argv:
        print(json.dumps(s, indent=1))
        return
    f = s["fg_latency_ms"]
    print(f"fg ops {f['n']}: p50 {f.get('p50', 0):.3f} p95 {f.get('p95', 0):.3f} "
          f"p99 {f.get('p99', 0):.3f} max {f.get('max', 0):.3f} ms; errors {s['fg_errors']}")
    print(f"bg throughput {s['bg_mb_per_s']:.2f} MB/s")
    b = s.get("block")
    if b:
        print(f"block requests {b['requests']} ({b['requests_per_s']:.0f}/s), modeled movement "
              f"{b['modeled_movement_blocks']} blocks")
        for role, w in b["wait_ms"].items():
            print(f"  {role} queue wait: n {w['n']} p50 {w['p50']:.3f} p95 {w['p95']:.3f} "
                  f"p99 {w['p99']:.3f} max {w['max']:.3f} ms")
        sv = b["service_ms"]
        print(f"  service time: p50 {sv['p50']:.3f} p99 {sv['p99']:.3f} ms")
        q = b["queue_depth"]
        print(f"queue depth mean {q['mean']:.2f} max {q['max']}; picks with >=2 pending "
              f"{q['share_picked_with_2plus']:.1%}")
        print(f"fg requests that found bg requests waiting: "
              f"{b['fg_requests_finding_bg_waiting']:.1%}")
        r, sh = b["fg_requests_per_op"], b["fg_op_share_in_block_layer"]
        print(f"block requests per fg op: mean {r.get('mean', 0):.1f}; share of fg op time in "
              f"block layer: mean {sh.get('mean', 0):.1%} p50 {sh.get('p50', 0):.1%}")


if __name__ == "__main__":
    main()
