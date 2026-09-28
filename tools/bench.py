#!/usr/bin/env python3
"""Run the bench_* programs against glibc, mimalloc and MemLoc. (D-30)

    tools/bench.py                       # full run, ~15 min, writes bench/results/
    tools/bench.py --runs 1 --threads 1 4 --out /tmp/quick

The allocator is chosen with LD_PRELOAD, so all three run the same binaries.
Raw rows go to results.csv (bench,allocator,threads,metric,run,value), and
machine.json records where they were measured. tools/plot_bench.py turns
both into the README's graphs.
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = Path.home() / "build" / "memloc"


def find_mimalloc() -> str | None:
    for pattern in ("/usr/lib/*-linux-gnu/libmimalloc.so*", "/usr/local/lib/libmimalloc.so*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]
    return None


def run(cmd: list[str], preload: str | None) -> list[tuple[str, int, str, float]]:
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    if preload:
        env["LD_PRELOAD"] = preload
    out = subprocess.run(cmd, env=env, capture_output=True, text=True, check=True, timeout=600)
    rows = []
    for line in out.stdout.splitlines():
        bench, threads, metric, value = line.split(",")
        rows.append((bench, int(threads), metric, float(value)))
    return rows


def sh(cmd: str) -> str:
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def machine_info(allocators: dict[str, str | None]) -> dict:
    lscpu = {}
    for line in sh("lscpu").splitlines():
        key, _, value = line.partition(":")
        lscpu[key.strip()] = value.strip()
    mem_kb = int(sh("awk '/MemTotal/ {print $2}' /proc/meminfo") or 0)
    return {
        "date": time.strftime("%Y-%m-%d"),
        "cpu": lscpu.get("Model name", platform.processor()),
        "logical_cpus": os.cpu_count(),
        "cores_per_socket": lscpu.get("Core(s) per socket"),
        "threads_per_core": lscpu.get("Thread(s) per core"),
        "memory_gib": round(mem_kb / 2**20, 1),
        "kernel": platform.release(),
        "wsl": "microsoft" in platform.release().lower(),
        "glibc": platform.libc_ver()[1],
        "compiler": sh("c++ --version | head -1"),
        "mimalloc": allocators.get("mimalloc"),
        "memloc_commit": sh(f"git -C {ROOT} rev-parse --short HEAD"),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", type=Path, default=DEFAULT_BUILD, help="CMake build directory")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8, 16])
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results")
    args = ap.parse_args()

    memloc = args.build / "src" / "libmemloc.so"
    if not memloc.exists():
        ap.error(f"{memloc} not found; build first (make build)")
    allocators: dict[str, str | None] = {"glibc": None, "mimalloc": find_mimalloc(),
                                         "memloc": str(memloc)}
    if allocators["mimalloc"] is None:
        print("mimalloc not found (apt install libmimalloc-dev); skipping it", file=sys.stderr)
        del allocators["mimalloc"]

    bench_dir = args.build / "bench"
    plan: list[tuple[str, int]] = []
    for t in args.threads:
        plan += [("larson", t), ("random", t)]
    plan += [("rss", 1), ("rss", 4)]
    plan.append(("realloc", 1))

    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    # Runs are the outer loop and allocators the inner one, so slow drift
    # (thermal, background load) hits all three allocators alike.
    for r in range(args.runs):
        for bench, threads in plan:
            for name, preload in allocators.items():
                for b, t, metric, value in run([str(bench_dir / f"bench_{bench}"), str(threads)], preload):
                    rows.append((b, name, t, metric, r, value))
            print(f"run {r + 1}/{args.runs}: {bench} x{threads} done", file=sys.stderr)

    with open(args.out / "results.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["bench", "allocator", "threads", "metric", "run", "value"])
        w.writerows(rows)
    with open(args.out / "machine.json", "w", encoding="utf-8") as f:
        json.dump(machine_info(allocators), f, indent=2)
        f.write("\n")
    print(f"wrote {args.out / 'results.csv'} ({len(rows)} rows) and machine.json", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
