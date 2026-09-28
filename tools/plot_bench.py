#!/usr/bin/env python3
"""Draw the README's benchmark graphs from bench/results/. (D-32)

    tools/plot_bench.py                          # bench/results -> docs/img
    tools/plot_bench.py --results /tmp/quick --out /tmp/img
    tools/plot_bench.py --table                  # also print the markdown tables

Every number comes from results.csv. The median over runs is drawn, and
throughput also shows the min-max band. Each allocator keeps one colour in
every chart (validated for colour-vision deficiency, 3 slots all-pairs), and
lines are labelled directly as well as in the legend.
"""
from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ORDER = ["memloc", "mimalloc", "glibc"]
LABEL = {"memloc": "MemLoc", "mimalloc": "mimalloc", "glibc": "glibc"}
COLOR = {"memloc": "#2a78d6", "mimalloc": "#eb6834", "glibc": "#1baf7a"}
SURFACE, INK, INK2, MUTED, GRID, AXIS = ("#fcfcfb", "#0b0b0b", "#52514e", "#898781",
                                         "#e1e0d9", "#c3c2b7")

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "font.family": "sans-serif", "font.size": 10, "text.color": INK,
    "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "axes.titlecolor": INK,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.titlelocation": "left",
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "axes.grid.axis": "y", "axes.axisbelow": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK2,
    "ytick.labelcolor": INK2, "legend.frameon": False, "legend.labelcolor": INK2,
})


def load(results: Path) -> dict:
    """{(bench, allocator, threads, metric): [values over runs]}"""
    data: dict = defaultdict(list)
    with open(results / "results.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            data[(r["bench"], r["allocator"], int(r["threads"]), r["metric"])].append(float(r["value"]))
    return data


def allocators(data: dict) -> list[str]:
    present = {k[1] for k in data}
    return [a for a in ORDER if a in present]


def spread_labels(ys: list[float], gap: float) -> list[float]:
    """Nudges label positions apart so no two are closer than gap."""
    order = sorted(range(len(ys)), key=lambda i: ys[i])
    out = list(ys)
    for prev, cur in zip(order, order[1:]):
        out[cur] = max(out[cur], out[prev] + gap)
    return out


def throughput(ax, data: dict, bench: str, title: str) -> None:
    ends = []
    for a in allocators(data):
        pts = sorted((t, v) for (b, al, t, m), v in data.items()
                     if b == bench and al == a and m == "mops")
        xs = [t for t, _ in pts]
        med = [statistics.median(v) for _, v in pts]
        ax.fill_between(xs, [min(v) for _, v in pts], [max(v) for _, v in pts],
                        color=COLOR[a], alpha=0.15, linewidth=0)
        ax.plot(xs, med, color=COLOR[a], linewidth=2, marker="o", markersize=5,
                markeredgecolor=SURFACE, markeredgewidth=1.5, label=LABEL[a])
        ends.append((a, xs[-1], med[-1]))
    top = max(e[2] for e in ends)
    for (a, x, y), ly in zip(ends, spread_labels([e[2] for e in ends], top * 0.07)):
        ax.annotate(f"{LABEL[a]} {y:.0f}", (x, y), xytext=(x * 1.12, ly), textcoords="data",
                    va="center", color=INK2, fontsize=9, annotation_clip=False)
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4, 8, 16], labels=["1", "2", "4", "8", "16"])
    ax.minorticks_off()
    ax.set_ylim(bottom=0)
    ax.set_xlabel("threads")
    ax.set_ylabel("million ops/s (median; band = min-max)")
    ax.set_title(title)


def bars(ax, groups: list[str], values: dict[str, list[float]], fmt: str, title: str,
         ylabel: str) -> None:
    names = [a for a in ORDER if a in values]
    width = 0.8 / len(names)
    for i, a in enumerate(names):
        xs = [g + (i - (len(names) - 1) / 2) * width for g in range(len(groups))]
        rects = ax.bar(xs, values[a], width, color=COLOR[a], edgecolor=SURFACE, linewidth=2,
                       label=LABEL[a])
        ax.bar_label(rects, labels=[fmt.format(v) for v in values[a]], padding=2,
                     color=INK2, fontsize=8.5)
    ax.set_xticks(range(len(groups)), labels=groups)
    ax.tick_params(axis="x", length=0)
    ax.set_ylim(bottom=0, top=max(max(v) for v in values.values()) * 1.15)
    ax.set_ylabel(ylabel)
    ax.set_title(title)


def single_bars(ax, values: dict[str, float], fmt: str, title: str, ylabel: str) -> None:
    """One bar per allocator, named on the axis (no legend needed)."""
    names = [a for a in ORDER if a in values]
    rects = ax.bar(range(len(names)), [values[a] for a in names], 0.6,
                   color=[COLOR[a] for a in names], edgecolor=SURFACE, linewidth=2)
    ax.bar_label(rects, labels=[fmt.format(values[a]) for a in names], padding=2, color=INK2,
                 fontsize=9)
    ax.set_xticks(range(len(names)), labels=[LABEL[a] for a in names])
    ax.tick_params(axis="x", length=0)
    ax.set_ylim(bottom=0, top=max(values.values()) * 1.15)
    ax.set_ylabel(ylabel)
    ax.set_title(title)


def footer(fig, machine: dict) -> None:
    text = (f"{machine.get('cpu', '?')}, {machine.get('logical_cpus', '?')} logical CPUs, "
            f"{machine.get('memory_gib', '?')} GiB, Linux {machine.get('kernel', '?')}"
            f"{' (WSL2)' if machine.get('wsl') else ''}, glibc {machine.get('glibc', '?')}; "
            f"MemLoc {machine.get('memloc_commit', '?')}")
    fig.text(0.01, 0.01, text, color=MUTED, fontsize=7.5)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--results", type=Path, default=ROOT / "bench" / "results")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "img")
    ap.add_argument("--table", action="store_true", help="print markdown tables of the medians")
    args = ap.parse_args()
    data = load(args.results)
    machine = json.loads((args.results / "machine.json").read_text(encoding="utf-8"))
    args.out.mkdir(parents=True, exist_ok=True)
    med = {k: statistics.median(v) for k, v in data.items()}
    names = allocators(data)

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    throughput(axes[0], data, "larson", "Larson (16-256 B, cross-thread frees)")
    throughput(axes[1], data, "random", "Random sizes (8 B - 32 KiB, log-uniform)")
    axes[0].legend(loc="upper left")
    fig.subplots_adjust(left=0.07, right=0.89, wspace=0.42, bottom=0.18)
    footer(fig, machine)
    fig.savefig(args.out / "throughput.png", dpi=150)
    plt.close(fig)

    phases = ["fill", "free", "refill"]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    for ax, t in zip(axes, (1, 4)):
        values = {a: [med[("rss", a, t, f"ratio_{p}")] for p in phases] for a in names}
        bars(ax, ["fill 256 MiB", "free half", "refill 128 MiB"], values, "{:.2f}",
             f"RSS / live bytes, {t} thread{'s' if t > 1 else ''} (lower is better)",
             "resident / live (1.0 = no overhead)")
    axes[0].legend(loc="upper left")
    fig.subplots_adjust(left=0.07, right=0.98, wspace=0.25, bottom=0.18)
    footer(fig, machine)
    fig.savefig(args.out / "rss.png", dpi=150)
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(11, 3.8))
    single_bars(axes[0], {a: med[("realloc", a, 1, "ns_per_call")] for a in names}, "{:.0f} ns",
                "realloc growth: time per call (lower is better)", "ns per realloc (median)")
    single_bars(axes[1], {a: med[("realloc", a, 1, "max_rss_mib")] for a in names}, "{:.0f} MiB",
                "realloc growth: peak RSS (64 MiB live at the end)", "peak RSS, MiB (median)")
    fig.subplots_adjust(left=0.07, right=0.98, wspace=0.3, bottom=0.18)
    footer(fig, machine)
    fig.savefig(args.out / "realloc.png", dpi=150)
    plt.close(fig)

    if args.table:
        threads = sorted({k[2] for k in data if k[0] == "larson"})
        for bench in ("larson", "random"):
            print(f"\n{bench} (M ops/s, median)\n\n| threads | " +
                  " | ".join(LABEL[a] for a in names) + " |\n|---|" + "---|" * len(names))
            for t in threads:
                print(f"| {t} | " + " | ".join(f"{med[(bench, a, t, 'mops')]:.0f}" for a in names) + " |")
        for t in (1, 4):
            print(f"\nrss ratio, {t} thread(s)\n\n| phase | " + " | ".join(LABEL[a] for a in names) +
                  " |\n|---|" + "---|" * len(names))
            for p in phases:
                print(f"| {p} | " + " | ".join(f"{med[('rss', a, t, f'ratio_{p}')]:.2f}" for a in names) + " |")
        print("\nrealloc\n\n| | " + " | ".join(LABEL[a] for a in names) + " |\n|---|" + "---|" * len(names))
        for metric, fmt in (("ns_per_call", "{:.0f} ns"), ("max_rss_mib", "{:.0f} MiB")):
            print(f"| {metric} | " + " | ".join(fmt.format(med[("realloc", a, 1, metric)]) for a in names) + " |")
    print(f"wrote throughput.png, rss.png, realloc.png to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
