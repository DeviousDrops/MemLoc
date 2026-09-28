#!/usr/bin/env python3
"""Run a tests/bugs program under MEMLOC_DEBUG=1 and check its report. (D-27)

    tools/run_bugs.py --lib build/src/libmemloc.so --bin-dir build/tests/bugs
    tools/run_bugs.py --lib ... --bin build/tests/bugs/bug_off_by_one --name off_by_one

A program passes when it dies the expected way (signal, or a clean exit for
leaks), its report contains a record of the expected type, and that record's
stack names the function recorded in tests/bugs/expected.json. With --keep,
the symbolised reports are saved (the Phase 6 triage evaluation reads them).
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from symbolize import read_records, symbolize_records  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
REQUIRED_KEYS = ("type", "pid", "tid", "program", "detail", "heap")


def frame_names(frames: list[dict]) -> set[str]:
    names = set()
    for f in frames:
        for name in (f.get("function"), f.get("symbol")):
            if name:
                names.add(name.split("(")[0])  # demangled C++ carries its signature
    return names


def run_one(lib: str, binary: str, name: str, spec: dict, keep: str | None) -> list[str]:
    """Returns a list of problems; empty means the bug was caught as expected."""
    with tempfile.TemporaryDirectory() as tmp:
        report = os.path.join(tmp, "report.jsonl")
        env = dict(os.environ, LD_PRELOAD=lib, MEMLOC_DEBUG="1", MEMLOC_REPORT=report)
        proc = subprocess.run([binary], env=env, capture_output=True, text=True, timeout=60)
        records = []
        if os.path.exists(report):
            with open(report, encoding="utf-8") as f:
                records = symbolize_records(read_records(f))
    if keep:
        os.makedirs(keep, exist_ok=True)
        with open(os.path.join(keep, f"{name}.jsonl"), "w", encoding="utf-8") as f:
            f.writelines(json.dumps(r) + "\n" for r in records)

    problems = []
    want_sig = spec["signal"]
    got = proc.returncode
    if want_sig is None and got != 0:
        problems.append(f"expected a clean exit, got status {got}")
    elif want_sig is not None and got != -getattr(signal, want_sig):
        problems.append(f"expected death by {want_sig}, got status {got}")

    for rec in records:
        missing = [k for k in REQUIRED_KEYS if k not in rec]
        if missing:
            problems.append(f"{rec.get('type')} record lacks {missing}")
    matching = [r for r in records if r["type"] == spec["type"]]
    if not matching:
        types = [r["type"] for r in records]
        problems.append(f"no {spec['type']} record (got {types}); stderr: {proc.stderr[-300:]}")
        return problems

    where = spec["in"]
    for func in spec.get("functions", [spec.get("function")]):
        if not any(func in frame_names(r.get(where, [])) for r in matching):
            seen = sorted(frame_names(matching[0].get(where, [])))
            problems.append(f"{func} not in {where} of any {spec['type']} record (saw {seen})")
    if "functions" in spec:
        summary = next((r for r in records if r["type"] == "leak-summary"), None)
        if summary is None or summary.get("sites") != len(spec["functions"]):
            problems.append(f"leak-summary should report {len(spec['functions'])} sites: {summary}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", required=True, help="path to libmemloc.so")
    ap.add_argument("--expected", default=str(ROOT / "tests/bugs/expected.json"))
    ap.add_argument("--bin", help="one bug program (use with --name)")
    ap.add_argument("--name", help="its key in expected.json")
    ap.add_argument("--bin-dir", help="directory holding bug_<name> for every entry")
    ap.add_argument("--keep", help="save symbolised reports to this directory")
    args = ap.parse_args()

    with open(args.expected, encoding="utf-8") as f:
        expected = json.load(f)
    if args.bin:
        jobs = [(args.name, args.bin)]
    elif args.bin_dir:
        jobs = [(n, os.path.join(args.bin_dir, f"bug_{n}")) for n in expected]
    else:
        ap.error("need --bin/--name or --bin-dir")

    failed = 0
    for name, binary in jobs:
        problems = run_one(args.lib, binary, name, expected[name], args.keep)
        print(f"{'PASS' if not problems else 'FAIL'}  {name}")
        for p in problems:
            print(f"      {p}")
        failed += bool(problems)
    print(f"{len(jobs) - failed}/{len(jobs)} bug programs caught")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
