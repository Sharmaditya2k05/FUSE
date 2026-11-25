#!/usr/bin/env python3
"""Turn benchmark.csv (the deterministic scheduler SIMULATION) into graphs.

Run `make bench` first, then `python3 scripts/plot_results.py`. Writes results/sim_*.png.
Everything here is simulated with a modeled service time; for measurements of the
mounted file system see scripts/report_results.py and results/mounted/.
"""
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SOURCE = Path("benchmark.csv")
OUT = Path("results")
POLICIES = ["fcfs", "sstf", "scan", "look", "adaptive", "deadline"]
COLORS = {"fcfs": "#8C8C8C", "sstf": "#2A6FDB", "scan": "#E08A1E", "look": "#B56B00",
          "adaptive": "#1A9E77", "deadline": "#C2362B"}
LABELS = {"fcfs": "FCFS", "sstf": "SSTF", "scan": "SCAN", "look": "LOOK",
          "adaptive": "Adaptive", "deadline": "Deadline"}
STATIC = ["sequential", "random", "clustered", "mixed", "interleaved"]

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9, "axes.spines.top": False,
                     "axes.spines.right": False, "legend.frameon": False})


def grouped(ax, rows, workloads, column, log=False):
    width = 0.8 / len(POLICIES)
    for i, p in enumerate(POLICIES):
        xs = [j + (i - (len(POLICIES) - 1) / 2) * width for j in range(len(workloads))]
        ys = [rows[(w, p)][column] for w in workloads]
        ax.bar(xs, ys, width, color=COLORS[p], label=LABELS[p])
    ax.set_xticks(range(len(workloads)))
    ax.set_xticklabels(workloads)
    ax.grid(axis="y", alpha=0.25)
    ax.set_axisbelow(True)
    if log:
        ax.set_yscale("log")


def main() -> None:
    if not SOURCE.exists():
        raise SystemExit("benchmark.csv not found; run ./schedfs-bench --all first")
    OUT.mkdir(exist_ok=True)
    rows = {}
    with open(SOURCE) as f:
        for r in csv.DictReader(f):
            rows[(r["workload"], r["policy"])] = {k: float(v) for k, v in r.items()
                                                  if k not in ("workload", "kind", "policy")}

    fig, axes = plt.subplots(1, 2, figsize=(13, 4.2))
    grouped(axes[0], rows, STATIC, "total_seek", log=True)
    axes[0].set_title("Modeled head movement (blocks, log scale)")
    grouped(axes[1], rows, STATIC, "maximum_wait_ms")
    axes[1].set_title("Longest simulated wait (ms)")
    axes[0].legend(ncol=3, fontsize=8)
    fig.suptitle("SIMULATION: fixed traces, all pending at t=0, head at 53 moving down; "
                 "service = 10 us + 2 us/block (modeled)", fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT / "sim_static.png", dpi=150)
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(13, 4.2))
    for ax, w, note in [(axes[0], "stream_far", "saturated nearby stream + a distant read "
                         "every 20 ms; constant 100 us service"),
                        (axes[1], "two_regions", "two sequential write streams + small reads "
                         "near the start; 50 us + 0.5 us/block")]:
        xs = list(range(len(POLICIES)))
        ax.bar(xs, [rows[(w, p)]["probe_max_wait_ms"] for p in POLICIES],
               color=[COLORS[p] for p in POLICIES])
        ax.set_xticks(xs)
        ax.set_xticklabels([LABELS[p] for p in POLICIES])
        ax.set_yscale("log")
        ax.set_ylabel("longest wait of the probe reads (ms, log)")
        for x, p in zip(xs, POLICIES):
            ax.annotate(f"{rows[(w, p)]['total_seek'] / 1000:.0f}k blocks moved",
                        (x, rows[(w, p)]["probe_max_wait_ms"]), ha="center", va="bottom",
                        fontsize=7, rotation=90, xytext=(0, 3), textcoords="offset points")
        ax.set_title(f"{w}: {note}", fontsize=8)
        ax.grid(axis="y", alpha=0.25)
    fig.suptitle("SIMULATION with a simulated clock (deterministic); not a measurement",
                 fontsize=9)
    fig.tight_layout()
    fig.savefig(OUT / "sim_dynamic.png", dpi=150)
    plt.close(fig)
    print(f"Generated {OUT}/sim_static.png and {OUT}/sim_dynamic.png")


if __name__ == "__main__":
    main()
