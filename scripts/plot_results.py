#!/usr/bin/env python3
from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt

source = Path("benchmark.csv")
if not source.exists():
    raise SystemExit("benchmark.csv not found; run ./schedfs-bench --all first")
out = Path("results"); out.mkdir(exist_ok=True)
df = pd.read_csv(source)
for col, title, ylabel in [
    ("total_seek", "Total seek distance", "blocks"),
    ("average_seek", "Average seek distance", "blocks/request"),
    ("average_wait_ms", "Average waiting time", "milliseconds"),
    ("maximum_wait_ms", "Maximum waiting time", "milliseconds"),
    ("throughput_rps", "Throughput", "requests/second")]:
    ax = df.pivot(index="workload", columns="policy", values=col).plot(kind="bar", figsize=(10, 5))
    ax.set_title(title); ax.set_ylabel(ylabel); ax.set_xlabel("Workload"); ax.grid(axis="y", alpha=.25)
    if col in {"total_seek", "average_seek"}: ax.set_yscale("log")
    plt.tight_layout(); plt.savefig(out / f"{col}.png", dpi=160); plt.close()
fig, ax = plt.subplots(figsize=(9, 5.5))
markers = {"fcfs":"o", "sstf":"s", "scan":"^", "adaptive":"D"}
for policy, group in df.groupby("policy"):
    ax.scatter(group.average_seek, group.average_wait_ms, label=policy.upper(), marker=markers[policy], s=42)
    for _, r in group.iterrows():
        ax.annotate(r.workload, (r.average_seek, r.average_wait_ms), xytext=(4, 4), textcoords="offset points", fontsize=7)
ax.set_xscale("log"); ax.set_xlabel("Average seek (blocks/request, log scale)"); ax.set_ylabel("Average wait (ms)")
ax.set_title("Seek distance versus waiting time"); ax.grid(alpha=.25); ax.legend(title="Policy")
plt.tight_layout(); plt.savefig(out / "seek_vs_wait.png", dpi=160); plt.close()
print(f"Generated graphs in {out}")
