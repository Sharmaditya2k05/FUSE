#!/usr/bin/env python3
"""Aggregate mounted experiment runs into tables and charts.

  python3 scripts/report_results.py results/mounted

Reads results/mounted/matrix/<config>/<workload>/<policy>/rep*/ (and .../host/,
.../tuning/) and writes:
  results/mounted/summary.csv   one row per config x workload x policy (machine-readable)
  results/mounted/summary.md    the same as Markdown tables
  results/mounted/*.png         charts (every chart says what is measured and what is modeled)

Latency percentiles are computed two ways: per repetition (mean and min..max over the
repetitions shows run-to-run variation) and pooled over all samples of all repetitions.
"""
import csv
import gzip
import json
import pathlib
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from analyze_run import pct  # noqa: E402

POLICIES = ["fcfs", "sstf", "scan", "look", "adaptive", "deadline"]
LABELS = {"fcfs": "FCFS", "sstf": "SSTF", "scan": "SCAN", "look": "LOOK",
          "adaptive": "Adaptive", "deadline": "Deadline", "host": "Host FS (no FUSE)"}
COLORS = {"fcfs": "#8C8C8C", "sstf": "#2A6FDB", "scan": "#E08A1E", "look": "#B56B00",
          "adaptive": "#1A9E77", "deadline": "#C2362B", "host": "#555555"}
WORKLOADS = ["bg2_read", "bg2_mix", "bg4_read", "copy2_read", "bg2_update", "light"]
WL_LABEL = {"bg2_read": "2 writers +\nsmall reads", "bg2_mix": "2 writers +\nreads/updates",
            "bg4_read": "4 writers +\nsmall reads", "copy2_read": "2 copiers +\nsmall reads",
            "bg2_update": "2 writers +\nsmall updates", "light": "no background\n(overhead)"}
CONFIG_LABEL = {"direct": "real I/O (O_DIRECT, no modeled delay)",
                "model": "modeled seek time (100 us + 0.25 us/block) on O_DIRECT"}

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9, "axes.spines.top": False,
                     "axes.spines.right": False, "legend.frameon": False})


def fg_samples(rep):
    meta = json.loads((rep / "workload.json").read_text())
    warm = meta["warmup_s"] * 1e9
    plain = rep / "fg.csv"
    with (open(plain) if plain.exists() else gzip.open(rep / "fg.csv.gz", "rt")) as f:
        return [int(r["total_ns"]) / 1e6 for r in csv.DictReader(f) if int(r["start_ns"]) >= warm]


def cell(dirpath):
    reps = sorted(p for p in dirpath.glob("rep*") if (p / "summary.json").exists())
    if not reps:
        return None
    sums = [json.loads((r / "summary.json").read_text()) for r in reps]
    pooled = [x for r in reps for x in fg_samples(r)]
    out = {"reps": len(reps), "fg_samples": len(pooled),
           "fg_p50_pooled": pct(pooled, 50), "fg_p95_pooled": pct(pooled, 95),
           "fg_p99_pooled": pct(pooled, 99), "fg_max": max(pooled)}
    for q in ["p50", "p95", "p99"]:
        vals = [s["fg_latency_ms"][q] for s in sums]
        out[f"fg_{q}_mean"] = statistics.mean(vals)
        out[f"fg_{q}_min"], out[f"fg_{q}_max"] = min(vals), max(vals)
    bg = [s["bg_mb_per_s"] for s in sums]
    out["bg_mb_s_mean"] = statistics.mean(bg)
    out["bg_mb_s_min"], out["bg_mb_s_max"] = min(bg), max(bg)
    out["fg_errors"] = sum(s["fg_errors"] for s in sums)
    blocks = [s["block"] for s in sums if "block" in s]
    if blocks:
        def m(f):
            vals = [f(b) for b in blocks]
            vals = [v for v in vals if v is not None]
            return statistics.mean(vals) if vals else None
        out["fg_wait_p99_ms"] = m(lambda b: b["wait_ms"].get("fg", {}).get("p99"))
        out["fg_wait_max_ms"] = max((b["wait_ms"].get("fg", {}).get("max") or 0) for b in blocks)
        out["bg_wait_p99_ms"] = m(lambda b: b["wait_ms"].get("bg", {}).get("p99"))
        out["bg_wait_max_ms"] = max((b["wait_ms"].get("bg", {}).get("max") or 0) for b in blocks)
        out["requests_per_s"] = m(lambda b: b["requests_per_s"])
        out["movement_blocks"] = m(lambda b: b["modeled_movement_blocks"])
        out["queue_depth_mean"] = m(lambda b: b["queue_depth"]["mean"])
        out["queue_depth_max"] = max(b["queue_depth"]["max"] or 0 for b in blocks)
        out["fg_found_bg_waiting"] = m(lambda b: b["fg_requests_finding_bg_waiting"])
        out["fg_block_share"] = m(lambda b: b["fg_op_share_in_block_layer"].get("mean"))
        out["fg_requests_per_op"] = m(lambda b: b["fg_requests_per_op"].get("mean"))
    return out


def fmt(v, d=2):
    return "" if v is None else (f"{v:.{d}f}" if isinstance(v, float) else str(v))


def bars(ax, table, cfg, key, lo, hi, ylabel, policies):
    width = 0.8 / len(policies)
    for i, p in enumerate(policies):
        xs, ys, errs = [], [], [[], []]
        for j, w in enumerate(WORKLOADS):
            c = table.get((cfg, w, p))
            if not c:
                continue
            xs.append(j + (i - (len(policies) - 1) / 2) * width)
            ys.append(c[key])
            errs[0].append(c[key] - c[lo])
            errs[1].append(c[hi] - c[key])
        ax.bar(xs, ys, width, color=COLORS[p], label=LABELS[p], yerr=errs, capsize=1.5,
               error_kw={"elinewidth": 0.7, "ecolor": "#333333"})
    ax.set_xticks(range(len(WORKLOADS)))
    ax.set_xticklabels([WL_LABEL[w] for w in WORKLOADS])
    ax.set_ylabel(ylabel)
    ax.grid(axis="y", alpha=0.25)
    ax.set_axisbelow(True)


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "results/mounted")
    table = {}
    for cfg in ["direct", "model"]:
        for w in WORKLOADS:
            for p in POLICIES:
                c = cell(root / "matrix" / cfg / w / p)
                if c:
                    table[(cfg, w, p)] = c
    for w in WORKLOADS:
        c = cell(root / "host" / w)
        if c:
            table[("host", w, "host")] = c
    if not table:
        raise SystemExit("no results found under " + str(root))

    cols = ["reps", "fg_samples", "fg_p50_mean", "fg_p95_mean", "fg_p99_mean", "fg_p99_min",
            "fg_p99_max", "fg_p95_pooled", "fg_p99_pooled", "fg_max", "bg_mb_s_mean",
            "bg_mb_s_min", "bg_mb_s_max", "fg_errors", "fg_wait_p99_ms", "fg_wait_max_ms",
            "bg_wait_p99_ms", "bg_wait_max_ms", "requests_per_s", "movement_blocks",
            "queue_depth_mean", "queue_depth_max", "fg_found_bg_waiting", "fg_block_share",
            "fg_requests_per_op"]
    with open(root / "summary.csv", "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["config", "workload", "policy"] + cols)
        for (cfg, w, p), c in sorted(table.items()):
            wr.writerow([cfg, w, p] + [c.get(k) for k in cols])

    md = ["# Mounted SchedFS experiments: summary", "",
          "Generated by `scripts/report_results.py` from the raw runs in this directory. "
          "Foreground latency is the end-to-end time of open + 4 KiB read/update + close, "
          "measured by the workload; background throughput is bytes written per second. "
          "`p99 (min..max)` is the 99th percentile of each repetition, averaged, with the "
          "range over repetitions; `pooled` uses all samples of all repetitions. Block-level "
          "columns come from the SchedFS trace. Movement is MODELED (block distance).", ""]
    for cfg in ["direct", "model", "host"]:
        rows = [(w, p, c) for (cf, w, p), c in table.items() if cf == cfg]
        if not rows:
            continue
        md += [f"## {CONFIG_LABEL.get(cfg, 'host Linux file system in the same container, no FUSE')}",
               "", "| workload | policy | fg p50 ms | fg p95 ms | fg p99 ms (min..max) | "
               "fg p99 pooled | bg MB/s (min..max) | fg block wait p99 / max ms | "
               "movement blocks | queue depth mean / max |",
               "|---|---|---|---|---|---|---|---|---|---|"]
        for w in WORKLOADS:
            for p in POLICIES + ["host"]:
                c = table.get((cfg, w, p))
                if not c:
                    continue
                md.append(
                    f"| {w} | {LABELS[p]} | {fmt(c['fg_p50_mean'])} | {fmt(c['fg_p95_mean'])} | "
                    f"{fmt(c['fg_p99_mean'])} ({fmt(c['fg_p99_min'])}..{fmt(c['fg_p99_max'])}) | "
                    f"{fmt(c['fg_p99_pooled'])} | {fmt(c['bg_mb_s_mean'])} "
                    f"({fmt(c['bg_mb_s_min'])}..{fmt(c['bg_mb_s_max'])}) | "
                    f"{fmt(c.get('fg_wait_p99_ms'))} / {fmt(c.get('fg_wait_max_ms'))} | "
                    f"{fmt(c.get('movement_blocks'), 0)} | {fmt(c.get('queue_depth_mean'))} / "
                    f"{fmt(c.get('queue_depth_max'))} |")
        md.append("")
    (root / "summary.md").write_text("\n".join(md) + "\n")

    for cfg in ["direct", "model"]:
        if not any(k[0] == cfg for k in table):
            continue
        fig, axes = plt.subplots(2, 1, figsize=(11, 7.5), sharex=True)
        bars(axes[0], table, cfg, "fg_p99_mean", "fg_p99_min", "fg_p99_max",
             "foreground p99 latency (ms)\nlower is better", POLICIES)
        bars(axes[1], table, cfg, "bg_mb_s_mean", "bg_mb_s_min", "bg_mb_s_max",
             "background throughput (MB/s)\nhigher is better", POLICIES)
        axes[0].set_title(f"Mounted SchedFS, {CONFIG_LABEL[cfg]}\n"
                          "measured; bars = mean of 3 runs, whiskers = min..max", fontsize=10)
        axes[0].legend(ncol=6, loc="upper left", fontsize=8)
        fig.tight_layout()
        fig.savefig(root / f"{cfg}_latency_throughput.png", dpi=150)
        plt.close(fig)

        # trade-off: one point per policy per workload (background-heavy workloads only)
        fig, axes = plt.subplots(1, 4, figsize=(14, 3.8))
        for ax, w in zip(axes, ["bg2_read", "bg4_read", "bg2_mix", "copy2_read"]):
            for p in POLICIES:
                c = table.get((cfg, w, p))
                if c:
                    ax.scatter(c["bg_mb_s_mean"], c["fg_p99_mean"], color=COLORS[p], s=40,
                               label=LABELS[p], edgecolor="white", zorder=3)
            ax.set_title(WL_LABEL[w].replace("\n", " "), fontsize=9)
            ax.set_xlabel("background MB/s (higher is better)")
            ax.grid(alpha=0.25)
        axes[0].set_ylabel("foreground p99 ms\n(lower is better)")
        axes[0].legend(fontsize=7)
        fig.suptitle(f"Trade-off, {CONFIG_LABEL[cfg]} (measured, mean of 3 runs)", fontsize=10)
        fig.tight_layout()
        fig.savefig(root / f"{cfg}_tradeoff.png", dpi=150)
        plt.close(fig)

        # latency distribution for the main workload
        fig, ax = plt.subplots(figsize=(7.5, 4.2))
        for p in POLICIES:
            d = root / "matrix" / cfg / "bg2_read" / p
            reps = sorted(d.glob("rep*"))
            xs = sorted(x for r in reps if (r / "fg.csv").exists() or (r / "fg.csv.gz").exists() for x in fg_samples(r))
            if xs:
                ax.plot(xs, [i / len(xs) for i in range(1, len(xs) + 1)], color=COLORS[p],
                        label=LABELS[p], lw=1.4)
        ax.set_xscale("log")
        ax.set_xlabel("foreground operation latency (ms, log scale)")
        ax.set_ylabel("fraction of operations")
        ax.set_ylim(0.5, 1.001)
        ax.set_title(f"2 background writers + small reads: latency CDF (upper half)\n"
                     f"{CONFIG_LABEL[cfg]}, all samples of 3 runs", fontsize=9)
        ax.grid(alpha=0.25, which="both")
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(root / f"{cfg}_bg2_read_cdf.png", dpi=150)
        plt.close(fig)

    # tuning sweep
    tuning = root / "tuning"
    if tuning.exists():
        fig, axes = plt.subplots(1, 2, figsize=(11, 3.8))
        for ax, cfg in zip(axes, ["direct", "model"]):
            ds, p99, mbs = [], [], []
            for d in ["0", "0.1", "0.25", "0.5", "1", "2", "5"]:
                c = cell(tuning / cfg / f"deadline_r{d}")
                if c:
                    ds.append(float(d))
                    p99.append(c["fg_p99_mean"])
                    mbs.append(c["bg_mb_s_mean"])
            if not ds:
                continue
            ax.plot(ds, p99, "o-", color=COLORS["deadline"], label="deadline: fg p99 (ms)")
            for p, ls in [("fcfs", "--"), ("sstf", ":")]:
                c = cell(tuning / cfg / p)
                if c:
                    ax.axhline(c["fg_p99_mean"], color=COLORS[p], ls=ls,
                               label=f"{LABELS[p]}: fg p99")
            ax2 = ax.twinx()
            ax2.plot(ds, mbs, "s-", color="#2A6FDB", alpha=0.6, label="deadline: bg MB/s")
            ax2.set_ylabel("background MB/s")
            ax.set_xscale("symlog", linthresh=0.1)
            ax.set_xlabel("read deadline (ms); write deadline 50 ms")
            ax.set_ylabel("foreground p99 (ms)")
            ax.set_title(f"Tuning (seed 101, not the evaluation seeds)\n{CONFIG_LABEL[cfg]}",
                         fontsize=8)
            ax.legend(fontsize=7, loc="upper left")
        fig.tight_layout()
        fig.savefig(root / "tuning.png", dpi=150)
        plt.close(fig)
    tw = root / "tuning_write"
    if tw.exists():
        fig, axes = plt.subplots(1, 2, figsize=(11, 3.8))
        for ax, cfg in zip(axes, ["direct", "model"]):
            for w, mk in [("bg2_read", "o-"), ("bg2_update", "s-")]:
                base = cell(tw / cfg / w / "fcfs")
                xs, ys = [], []
                for d in ["1", "2", "5", "10", "50"]:
                    c = cell(tw / cfg / w / f"deadline_w{d}")
                    if c and base:
                        xs.append(float(d))
                        ys.append(c["fg_p99_mean"] / base["fg_p99_mean"])
                if xs:
                    ax.plot(xs, ys, mk, label=f"{w}: deadline p99 / FCFS p99")
            ax.axhline(1.0, color=COLORS["fcfs"], ls="--", label="FCFS")
            ax.set_xscale("log")
            ax.set_xlabel("write deadline (ms); read deadline 0 ms")
            ax.set_ylabel("foreground p99 relative to FCFS")
            ax.set_title(f"Write-deadline tuning (seed 101)\n{CONFIG_LABEL[cfg]}", fontsize=8)
            ax.grid(alpha=0.25)
            ax.legend(fontsize=7)
        fig.tight_layout()
        fig.savefig(root / "tuning_write.png", dpi=150)
        plt.close(fig)
    print("wrote", root / "summary.csv", root / "summary.md", "and charts")


if __name__ == "__main__":
    main()
