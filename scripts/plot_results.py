#!/usr/bin/env python3
"""Turn benchmark.csv into the graphs in results/.

Run `make bench` first, then `python3 scripts/plot_results.py`.
"""
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

SOURCE = Path("benchmark.csv")
OUT = Path("results")

POLICIES = ["fcfs", "sstf", "scan", "adaptive"]
WORKLOADS = ["sequential", "random", "clustered", "mixed", "concurrent"]
# One fixed colour per policy so every graph reads the same way.
COLORS = {"fcfs": "#8C8C8C", "sstf": "#2A6FDB", "scan": "#E08A1E", "adaptive": "#1A9E77"}
LABELS = {"fcfs": "FCFS", "sstf": "SSTF", "scan": "SCAN", "adaptive": "Adaptive"}

METRICS = [
    ("total_seek", "Total seek distance", "blocks (log scale)", True),
    ("average_seek", "Average seek distance", "blocks / request (log scale)", True),
    ("average_wait_ms", "Average waiting time", "milliseconds", False),
    ("maximum_wait_ms", "Maximum waiting time", "milliseconds", False),
    ("throughput_rps", "Throughput", "requests / second", False),
]

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 10,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.titleweight": "bold",
    "axes.titlesize": 12,
    "legend.frameon": False,
})


def grouped_bars(ax, df, column, log):
    table = df.pivot(index="workload", columns="policy", values=column)
    table = table.reindex(index=WORKLOADS, columns=POLICIES)
    width = 0.2
    for i, policy in enumerate(POLICIES):
        xs = [x + (i - 1.5) * width for x in range(len(WORKLOADS))]
        ax.bar(xs, table[policy], width, color=COLORS[policy], label=LABELS[policy])
    ax.set_xticks(range(len(WORKLOADS)))
    ax.set_xticklabels([w.capitalize() for w in WORKLOADS])
    ax.grid(axis="y", alpha=0.25)
    ax.set_axisbelow(True)
    if log:
        ax.set_yscale("log")


def main() -> None:
    if not SOURCE.exists():
        raise SystemExit("benchmark.csv not found; run ./schedfs-bench --all first")
    OUT.mkdir(exist_ok=True)
    df = pd.read_csv(SOURCE)

    for column, title, ylabel, log in METRICS:
        fig, ax = plt.subplots(figsize=(10, 5))
        grouped_bars(ax, df, column, log)
        ax.set_title(title)
        ax.set_ylabel(ylabel)
        ax.set_xlabel("Workload")
        ax.legend(title="Policy", ncol=4, loc="upper left")
        fig.tight_layout()
        fig.savefig(OUT / f"{column}.png", dpi=160)
        plt.close(fig)

    # Trade-off view: lower-left is better on both axes.
    fig, ax = plt.subplots(figsize=(9, 5.5))
    markers = {"fcfs": "o", "sstf": "s", "scan": "^", "adaptive": "D"}
    for policy in POLICIES:
        group = df[df.policy == policy]
        ax.scatter(group.average_seek, group.average_wait_ms, label=LABELS[policy],
                   marker=markers[policy], s=60, color=COLORS[policy], edgecolor="white")
        for _, row in group.iterrows():
            ax.annotate(row.workload, (row.average_seek, row.average_wait_ms), xytext=(5, 4),
                        textcoords="offset points", fontsize=7, color="#555555")
    ax.set_xscale("log")
    ax.set_xlabel("Average seek (blocks / request, log scale)")
    ax.set_ylabel("Average wait (ms)")
    ax.set_title("Seek distance versus waiting time")
    ax.grid(alpha=0.25)
    ax.legend(title="Policy")
    fig.tight_layout()
    fig.savefig(OUT / "seek_vs_wait.png", dpi=160)
    plt.close(fig)

    # One-slide summary of the three headline metrics.
    fig, axes = plt.subplots(1, 3, figsize=(16, 4.8))
    for ax, (column, title, ylabel, log) in zip(axes, [METRICS[0], METRICS[2], METRICS[4]]):
        grouped_bars(ax, df, column, log)
        ax.set_title(title)
        ax.set_ylabel(ylabel)
        ax.tick_params(axis="x", labelrotation=20)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(OUT / "summary.png", dpi=160)
    plt.close(fig)

    print(f"Generated graphs in {OUT}/")


if __name__ == "__main__":
    main()
