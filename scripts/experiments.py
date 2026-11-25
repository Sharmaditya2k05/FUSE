#!/usr/bin/env python3
"""Run the mounted SchedFS experiments and keep machine-readable results.

  python3 scripts/experiments.py tune   OUTROOT [--reps 2]           (read deadline)
  python3 scripts/experiments.py tunew  OUTROOT [--reps 2]           (write deadline)
  python3 scripts/experiments.py matrix OUTROOT [--reps 3] [--duration 10]
  python3 scripts/experiments.py host   OUTROOT [--reps 3] [--duration 10]
  python3 scripts/experiments.py demo   OUTROOT                       (short, ~1 minute)

Every run uses a fresh 64 MiB image (scripts/run_mounted.sh), the same workload
specification for every policy, and a seed per repetition. After each run the trace is
summarized (scripts/analyze_run.py) into summary.json next to the raw CSV files; the
large per-request trace is deleted unless --keep-traces is given (the first repetition
of each cell keeps a gzipped copy when --keep-first-trace is given).

Tuning uses seed 101 and is kept apart from the evaluation, which uses seeds 1001+.
"""
import argparse
import gzip
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import analyze_run  # noqa: E402

# Storage configurations. "direct" reaches the (virtual) disk with O_DIRECT and adds no
# delay. "model" adds a MODELED seek time on top: 100 us + 0.25 us per block moved.
CONFIGS = {
    "direct": "--direct-io",
    "model": "--direct-io --model-base-us=100 --model-us-per-block=0.25",
}
POLICIES = {
    "fcfs": "--sched=fcfs",
    "sstf": "--sched=sstf",
    "scan": "--sched=scan",
    "look": "--sched=look",
    "adaptive": "--sched=adaptive",
    "deadline": "--sched=deadline",  # defaults: read 0 ms, write 2 ms (from tuning)
}
WORKLOADS = {
    "bg2_read": "--bg 2 --fg-op read",              # background writes, periodic small reads
    "bg2_mix": "--bg 2 --fg-op mix",                # small reads and updates
    "bg4_read": "--bg 4 --fg-op read",              # several competing background workers
    "light": "--bg 0 --fg-op read",                 # no background: overhead only
    "bg2_update": "--bg 2 --fg-op update",          # foreground writes (no read priority)
    "copy2_read": "--bg 2 --bg-mode copy --fg-op read",  # background reads compete too
}
COMMON = "--warmup 1 --fg-interval-ms 20 --fg-file-kb 64 --fg-io-kb 4 --bg-file-kb 2048 " \
         "--bg-chunk-kb 128"


def one_run(out, fsopts, wlopts, keep_trace=False):
    out = pathlib.Path(out)
    if (out / "summary.json").exists():
        return json.loads((out / "summary.json").read_text())
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(["bash", str(ROOT / "scripts/run_mounted.sh"), str(out), fsopts, wlopts,
                    "64M"], check=True, stdout=subprocess.DEVNULL)
    s = analyze_run.summarize(out)
    (out / "summary.json").write_text(json.dumps(s, indent=1))
    trace = out / "trace.csv"
    if trace.exists():
        if keep_trace:
            with open(trace, "rb") as f, gzip.open(out / "trace.csv.gz", "wb") as g:
                shutil.copyfileobj(f, g)
        trace.unlink()
    return s


def line(tag, s):
    f = s["fg_latency_ms"]
    print(f"{tag}: fg p50 {f['p50']:.2f} p95 {f['p95']:.2f} p99 {f['p99']:.2f} ms, "
          f"bg {s['bg_mb_per_s']:.2f} MB/s", flush=True)


def tune(args):
    root = pathlib.Path(args.outroot)
    wl = f"{COMMON} --duration {args.duration} --bg 2 --fg-op read --seed 101"
    for cfg, copts in CONFIGS.items():
        for rep in range(args.reps):
            line(f"tune {cfg} fcfs r{rep}", one_run(root / cfg / "fcfs" / f"rep{rep}",
                                                    f"--sched=fcfs {copts}", wl))
            line(f"tune {cfg} sstf r{rep}", one_run(root / cfg / "sstf" / f"rep{rep}",
                                                    f"--sched=sstf {copts}", wl))
            for d in ["0", "0.1", "0.25", "0.5", "1", "2", "5"]:
                fs = f"--sched=deadline --read-deadline-ms={d} --write-deadline-ms=50 {copts}"
                line(f"tune {cfg} read_deadline {d} r{rep}",
                     one_run(root / cfg / f"deadline_r{d}" / f"rep{rep}", fs, wl))


def tune_write(args):
    """Second tuning step: the write deadline, on reads and on updates (seed 101)."""
    root = pathlib.Path(args.outroot)
    for cfg, copts in CONFIGS.items():
        for wname in ["bg2_update", "bg2_read"]:
            wl = f"{COMMON} --duration {args.duration} {WORKLOADS[wname]} --seed 101"
            for rep in range(args.reps):
                line(f"tunew {cfg} {wname} fcfs r{rep}",
                     one_run(root / cfg / wname / "fcfs" / f"rep{rep}", f"--sched=fcfs {copts}", wl))
                for d in ["50", "10", "5", "2", "1"]:
                    fs = f"--sched=deadline --read-deadline-ms=0 --write-deadline-ms={d} {copts}"
                    line(f"tunew {cfg} {wname} write_deadline {d} r{rep}",
                         one_run(root / cfg / wname / f"deadline_w{d}" / f"rep{rep}", fs, wl))


def matrix(args):
    root = pathlib.Path(args.outroot)
    for cfg, copts in CONFIGS.items():
        for wname, wopts in WORKLOADS.items():
            for rep in range(args.reps):
                for pname, popts in POLICIES.items():
                    wl = f"{COMMON} --duration {args.duration} {wopts} --seed {1001 + rep}"
                    out = root / cfg / wname / pname / f"rep{rep}"
                    t = time.time()
                    s = one_run(out, f"{popts} {copts}", wl,
                                keep_trace=args.keep_first_trace and rep == 0 and
                                wname == "bg2_read")
                    line(f"{cfg}/{wname}/{pname}/rep{rep} ({time.time() - t:.0f}s)", s)


def host(args):
    """The same workload on the container's own Linux file system (no FUSE)."""
    root = pathlib.Path(args.outroot)
    for wname, wopts in WORKLOADS.items():
        for rep in range(args.reps):
            out = root / "host" / wname / f"rep{rep}"
            if (out / "summary.json").exists():
                continue
            out.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir="/var/tmp") as d:
                wl = f"{COMMON} --duration {args.duration} {wopts} --seed {1001 + rep}"
                subprocess.run([str(ROOT / "schedfs-workload"), "--dir", d, "--out", str(out)] +
                               wl.split(), check=True)
            s = analyze_run.summarize(out)
            (out / "summary.json").write_text(json.dumps(s, indent=1))
            line(f"host/{wname}/rep{rep}", s)


def demo(args):
    """A short live comparison for the presentation: FCFS versus deadline."""
    root = pathlib.Path(args.outroot)
    wl = f"{COMMON} --duration 8 --bg 2 --fg-op read --seed 7"
    for pname in ["fcfs", "deadline"]:
        line(f"demo model {pname}", one_run(root / pname, f"{POLICIES[pname]} {CONFIGS['model']}",
                                           wl))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("mode", choices=["tune", "tunew", "matrix", "host", "demo"])
    p.add_argument("outroot")
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--duration", type=float, default=10)
    p.add_argument("--keep-first-trace", action="store_true")
    a = p.parse_args()
    if a.mode in ("tune", "tunew") and a.reps == 3:
        a.reps, a.duration = 2, 8
    {"tune": tune, "tunew": tune_write, "matrix": matrix, "host": host, "demo": demo}[a.mode](a)


if __name__ == "__main__":
    main()
