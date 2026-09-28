#!/usr/bin/env python3
"""Add file:line information to MemLoc debug reports (JSON lines). (D-24)

The allocator records only module + offset for each frame, because DWARF
parsing does not belong inside a crashing process. This script runs
addr2line once per module and adds "function", "file" and "line" to every
frame of "stack", "alloc_stack" and "free_stack".

    tools/symbolize.py report.jsonl            # JSON lines on stdout
    tools/symbolize.py --text < report.jsonl   # human-readable
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict

STACK_KEYS = ("stack", "alloc_stack", "free_stack")


def _addr2line(module: str, offsets: list[str]) -> list[tuple[str, str, int | None]]:
    """(function, file, line) for each offset; '??' when unknown."""
    unknown = [("??", "??", None)] * len(offsets)
    if not os.path.exists(module):
        return unknown
    try:
        out = subprocess.run(
            ["addr2line", "-C", "-f", "-e", module, *offsets],
            capture_output=True, text=True, check=True, timeout=60,
        ).stdout.splitlines()
    except (OSError, subprocess.SubprocessError):
        return unknown
    result = []
    for i in range(len(offsets)):
        func = out[2 * i] if 2 * i < len(out) else "??"
        loc = out[2 * i + 1] if 2 * i + 1 < len(out) else "??:0"
        path, _, line = loc.partition(":")
        line = line.split(" ")[0]  # strip " (discriminator N)"
        result.append((func, path, int(line) if line.isdigit() and line != "0" else None))
    return result


def symbolize_records(records: list[dict]) -> list[dict]:
    """Annotates the frames of every record in place and returns the list."""
    wanted: dict[str, set[str]] = defaultdict(set)
    for rec in records:
        for key in STACK_KEYS:
            for frame in rec.get(key, []):
                if frame.get("module"):
                    wanted[frame["module"]].add(frame["offset"])
    resolved: dict[tuple[str, str], tuple[str, str, int | None]] = {}
    for module, offsets in wanted.items():
        ordered = sorted(offsets)
        for off, info in zip(ordered, _addr2line(module, ordered)):
            resolved[(module, off)] = info
    for rec in records:
        for key in STACK_KEYS:
            for frame in rec.get(key, []):
                func, path, line = resolved.get((frame.get("module"), frame.get("offset")),
                                                ("??", "??", None))
                if func == "??" and frame.get("symbol"):
                    func = frame["symbol"]
                frame["function"] = func
                frame["file"] = None if path == "??" else path
                frame["line"] = line
    return records


def read_records(stream) -> list[dict]:
    records = []
    for line in stream:
        line = line.strip()
        if line.startswith("{"):
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue  # a truncated line from a crash mid-write
    return records


def format_text(rec: dict) -> str:
    head = f"{rec['type']}"
    if "address" in rec:
        head += f" at {rec['address']}"
    lines = [f"== {head}: {rec.get('detail', '')}"]
    for key in STACK_KEYS:
        frames = rec.get(key)
        if not frames:
            continue
        tid = rec.get(key.replace("stack", "tid")) if key != "stack" else rec.get("tid")
        lines.append(f"  {key} (thread {tid}):")
        for i, f in enumerate(frames):
            where = f"{f['file']}:{f['line']}" if f.get("file") else os.path.basename(
                f.get("module") or "?")
            lines.append(f"    #{i:<2} {f.get('function', '??')}  {where}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("reports", nargs="*", help="report files (default: stdin)")
    ap.add_argument("--text", action="store_true", help="print a human-readable report")
    args = ap.parse_args()
    records: list[dict] = []
    if args.reports:
        for path in args.reports:
            with open(path, encoding="utf-8") as f:
                records += read_records(f)
    else:
        records = read_records(sys.stdin)
    for rec in symbolize_records(records):
        print(format_text(rec) if args.text else json.dumps(rec))
    return 0


if __name__ == "__main__":
    sys.exit(main())
