#!/usr/bin/env python3
"""Triage MemLoc debug reports with an LLM. (D-35 to D-39)

    tools/triage.py report r.jsonl ...        # group, then root cause + fix per group
    tools/triage.py eval --reports DIR        # score against tests/bugs/expected.json
    --llm auto|mock|claude                    # auto: Claude if ANTHROPIC_API_KEY is set

Reports should be symbolized (tools/symbolize.py, or `make bugs`) so frames
carry function, file and line. Every model call is appended to the trace
file as one JSON line. The API key is read from the environment by the SDK.

    reports ──> signature ──> groups ──> prompt (report + source) ──> LLM
                (D-35)                                   JSON schema (D-36)
                                                               │
                expected.json ──> exact match + judge (D-38) <─┘
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODEL = "claude-opus-5"
STACKS = ("stack", "alloc_stack", "free_stack")
SYSTEM_MODULE = re.compile(r"^(/usr)?/lib|/ld-linux|/libmemloc")
BUG_TYPES = ["heap-buffer-overflow", "heap-buffer-underflow", "double-free", "use-after-free",
             "invalid-free", "leak", "segv", "other"]


def schema(**props: dict) -> dict:
    """Strict object schema: every property required, nothing else allowed (D-36)."""
    return {"type": "object", "properties": props, "required": list(props),
            "additionalProperties": False}


RESULT_SCHEMA = schema(
    bug_type={"type": "string", "enum": BUG_TYPES},
    culprit_function={"type": "string", "description": "the function in the stacks whose code is wrong"},
    root_cause={"type": "string", "description": "why the bug happens, 1-3 sentences"},
    fix={"type": "string", "description": "the smallest correct code change"},
    confidence={"type": "string", "enum": ["low", "medium", "high"]})
JUDGE_SCHEMA = schema(root_cause_correct={"type": "boolean"}, fix_correct={"type": "boolean"},
                      reason={"type": "string"})
SYSTEM_PROMPT = """You triage memory-safety reports from MemLoc, a malloc replacement whose \
debug mode detects heap overflows, use-after-free, double and invalid frees, leaks and \
segfaults. You get one report (stacks innermost first: `stack` is where it was detected, \
`alloc_stack` and `free_stack` are where the block came from and went) and the source \
around each frame. Find the root cause in the program, not in the allocator, and propose \
the smallest correct fix. Name the culprit exactly as the function appears in the stacks. \
If the evidence is thin, say so and lower your confidence."""
JUDGE_PROMPT = """You grade a bug triage answer against a known ground truth. The root cause is \
correct if it identifies the same defect (paraphrase is fine; a vaguer or different mechanism \
is not). The fix is correct if applying it would remove the defect without breaking the \
program, even if it differs from the reference fix."""


def user_frames(frames: list[dict] | None) -> list[dict]:
    """Frames in the program itself: not libc, the loader, MemLoc or _start."""
    return [f for f in frames or [] if f.get("module") and not SYSTEM_MODULE.search(f["module"])
            and f.get("function") != "_start"]


def frame_name(f: dict) -> str:
    name = f.get("function") or f.get("symbol")
    return name.split("(")[0] if name else f"{Path(f['module']).name}+{f.get('offset')}"


def signature(rec: dict) -> str:
    """Stable across ASLR, pids and rebuilds: type + innermost user functions per stack."""
    parts = [rec.get("type", "?")] + [s + ":" + ",".join(frame_name(f) for f in user_frames(rec.get(s))[:3])
                                      for s in STACKS]
    return hashlib.sha1("|".join(parts).encode()).hexdigest()[:12]


def load_reports(paths: list[Path]) -> list[dict]:
    recs = [json.loads(ln) for p in paths for ln in p.read_text(encoding="utf-8").splitlines() if ln.strip()]
    return [r for r in recs if r.get("type") != "leak-summary"]  # repeats the per-site leak records


def group(recs: list[dict]) -> dict[str, dict]:
    groups: dict[str, dict] = {}
    for rec in recs:
        g = groups.setdefault(signature(rec), {"signature": signature(rec), "count": 0, "report": rec})
        g["count"] += 1
    return groups


def resolve(path: str) -> Path | None:
    """The recorded source path, or the same path re-rooted in this checkout."""
    p = Path(path)
    candidates = [p] + [ROOT.joinpath(*p.parts[i:]) for i in range(1, len(p.parts))]
    return next((c for c in candidates if c.is_file()), None)


def source_context(rec: dict, radius: int = 12, limit: int = 6) -> list[str]:
    out, seen = [], set()
    for s in STACKS:
        for f in user_frames(rec.get(s)):
            key = (f.get("file"), f.get("line"))
            if not all(key) or key in seen or not (p := resolve(f["file"])):
                continue
            seen.add(key)
            lines, line = p.read_text(encoding="utf-8", errors="replace").splitlines(), f["line"]
            body = "\n".join(f"{n + 1:4}{'>' if n + 1 == line else ' '} {lines[n]}"
                             for n in range(max(0, line - 1 - radius), min(len(lines), line + radius)))
            out.append(f"--- {p.name}:{line} ({frame_name(f)})\n{body}")
    return out[:limit]


def describe(rec: dict) -> str:
    lines = [f"type: {rec.get('type')}", f"detail: {rec.get('detail')}", f"thread: {rec.get('tid')}"]
    for s in STACKS:
        frames = user_frames(rec.get(s))
        if frames:
            lines.append(f"{s}:")
            lines += [f"  #{i} {frame_name(f)} {Path(f.get('file') or '?').name}:{f.get('line')}"
                      for i, f in enumerate(frames)]
    return "\n".join(lines)


def build_prompt(g: dict) -> str:
    ctx = "\n\n".join(source_context(g["report"])) or "(no source found)"
    return f"This report occurred {g['count']} time(s).\n\n{describe(g['report'])}\n\nSource:\n{ctx}"


def check_result(d: dict) -> None:
    for key in RESULT_SCHEMA["required"]:
        if not isinstance(d.get(key), str) or not d[key]:
            raise ValueError(f"field {key!r} missing or empty")
    if d["bug_type"] not in BUG_TYPES or d["confidence"] not in ("low", "medium", "high"):
        raise ValueError("bug_type or confidence outside its enum")


class Trace:
    def __init__(self, path: Path | None):
        self.path = path

    def log(self, **entry) -> None:
        if self.path:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(json.dumps({"time": time.strftime("%Y-%m-%dT%H:%M:%S"), **entry}) + "\n")


def recall(answer: str, truth: str) -> float:
    """Share of the truth's keywords that the answer also uses (the offline judge)."""
    words = lambda s: set(re.findall(r"[a-z_]{4,}", s.lower())) - {"that", "with", "from", "this"}
    return len(words(answer) & words(truth)) / max(1, len(words(truth)))


class MockLLM:
    """Rule-based baseline: blame the innermost user frame of the stack the type points at."""
    name = "mock"
    PICK = {"heap-buffer-overflow": "alloc_stack", "heap-buffer-underflow": "alloc_stack",
            "leak": "alloc_stack", "use-after-free": "free_stack"}

    def triage(self, prompt: str, g: dict) -> tuple[dict, dict]:
        rec = g["report"]
        kind = rec.get("type") if rec.get("type") in BUG_TYPES else "other"
        frames = user_frames(rec.get(self.PICK.get(kind, "stack"))) or user_frames(rec.get("stack"))
        culprit = frame_name(frames[0]) if frames else "unknown"
        where = f"{Path(frames[0].get('file') or '?').name}:{frames[0].get('line')}" if frames else "?"
        return {"bug_type": kind, "culprit_function": culprit,
                "root_cause": f"{rec.get('detail')} (see {culprit} at {where}).",
                "fix": f"Check the size and lifetime of the block in {culprit}.",
                "confidence": "low"}, {"model": "mock"}

    def judge(self, prompt: str, answer: dict, truth: dict) -> tuple[dict, dict]:
        return {"root_cause_correct": recall(answer["root_cause"], truth["root_cause"]) >= 0.4,
                "fix_correct": recall(answer["fix"], truth["fix"]) >= 0.4,
                "reason": "keyword recall >= 0.4"}, {"model": "mock"}


class ClaudeLLM:
    name = "claude"

    def __init__(self, model: str = MODEL, client=None):
        try:  # only needed for real calls; the tests pass a fake client
            import anthropic
        except ImportError:
            sys.exit("triage: --llm claude needs the SDK: pip install -r tools/requirements-triage.txt")
        self.anthropic, self.model = anthropic, model
        self.client = client or anthropic.Anthropic()

    def _call(self, system: str, prompt: str, schema: dict, max_tokens: int) -> tuple[dict, dict]:
        start = time.monotonic()
        try:
            resp = self.client.beta.messages.create(
                model=self.model, max_tokens=max_tokens, system=system,
                messages=[{"role": "user", "content": prompt}],
                thinking={"type": "adaptive"},
                output_config={"format": {"type": "json_schema", "schema": schema}},
                betas=["server-side-fallback-2026-07-01"], fallbacks="default")
        except self.anthropic.AuthenticationError:
            raise  # no point trying the other groups
        except self.anthropic.APIError as e:  # status, rate limit, connection; SDK already retried
            raise RuntimeError(f"{type(e).__name__}: {e}") from e
        meta = {"model": getattr(resp, "model", self.model), "stop_reason": resp.stop_reason,
                "latency_s": round(time.monotonic() - start, 2),
                "usage": {k: getattr(resp.usage, k, None) for k in ("input_tokens", "output_tokens")}}
        if resp.stop_reason != "end_turn":  # refusal or max_tokens: no complete JSON
            raise RuntimeError(f"stop_reason {resp.stop_reason}: {meta}")
        text = next(b.text for b in resp.content if b.type == "text")
        return json.loads(text), {**meta, "raw": text}

    def triage(self, prompt: str, g: dict) -> tuple[dict, dict]:
        return self._call(SYSTEM_PROMPT, prompt, RESULT_SCHEMA, 16000)

    def judge(self, prompt: str, answer: dict, truth: dict) -> tuple[dict, dict]:
        return self._call(JUDGE_PROMPT, prompt, JUDGE_SCHEMA, 4000)


def ask(llm, kind: str, prompt: str, trace: Trace, *args, **log) -> tuple[dict | None, str | None]:
    """One logged call to llm.triage or llm.judge. A failure is recorded, not raised."""
    result, meta, error = None, {}, None
    try:
        result, meta = getattr(llm, kind)(prompt, *args)
        if kind == "triage":
            check_result(result)
    except (RuntimeError, ValueError, KeyError, StopIteration, json.JSONDecodeError) as e:
        result, error = None, f"{type(e).__name__}: {e}"
    trace.log(kind=kind, llm=llm.name, prompt=prompt, result=result, meta=meta, error=error, **log)
    return result, error


def triage_group(llm, g: dict, trace: Trace) -> dict:
    result, error = ask(llm, "triage", build_prompt(g), trace, g, signature=g["signature"], count=g["count"])
    return {"signature": g["signature"], "count": g["count"], "type": g["report"].get("type"),
            "result": result, "error": error}


def evaluate(llm, reports: Path, expected: dict, trace: Trace) -> dict:
    rows = []
    for name, truth in expected.items():
        path = reports / f"{name}.jsonl"
        groups = [g for g in group(load_reports([path])).values()
                  if g["report"].get("type") == truth["type"]] if path.exists() else []
        done = [r["result"] for r in (triage_group(llm, g, trace) for g in groups) if r["result"]]
        complete = bool(groups) and len(done) == len(groups)
        want = set(truth.get("functions") or [truth["function"]])
        row = {"bug": name, "groups": len(groups),
               "type": complete and all(r["bug_type"] == truth["type"] for r in done),
               "culprit": complete and {r["culprit_function"] for r in done} >= want}
        answer = {k: " / ".join(r[k] for r in done) for k in ("root_cause", "fix")}
        prompt = (f"Ground truth:\nroot cause: {truth['root_cause']}\nfix: {truth['fix']}\n\n"
                  f"Answer:\nroot cause: {answer['root_cause']}\nfix: {answer['fix']}")
        verdict = (ask(llm, "judge", prompt, trace, answer, truth, bug=name)[0] if complete else None) or {}
        rows.append({**row, "root_cause": bool(verdict.get("root_cause_correct")),
                     "fix": bool(verdict.get("fix_correct"))})
    totals = {k: sum(r[k] for r in rows) for k in ("type", "culprit", "root_cause", "fix")}
    return {"llm": llm.name, "cases": len(rows), "totals": totals, "rows": rows}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("command", choices=["report", "eval"])
    ap.add_argument("files", nargs="*", type=Path, help="report .jsonl files (report)")
    ap.add_argument("--reports", type=Path, help="directory of <bug>.jsonl reports (eval)")
    ap.add_argument("--expected", type=Path, default=ROOT / "tests" / "bugs" / "expected.json")
    ap.add_argument("--llm", choices=["auto", "mock", "claude"], default="auto")
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--trace", type=Path, help="append one JSON line per model call here")
    ap.add_argument("--json", action="store_true", help="print JSON instead of text")
    args = ap.parse_args()
    kind = args.llm if args.llm != "auto" else "claude" if os.environ.get("ANTHROPIC_API_KEY") else "mock"
    llm, trace = ClaudeLLM(args.model) if kind == "claude" else MockLLM(), Trace(args.trace)

    if args.command == "report":
        if not args.files:
            ap.error("report needs at least one .jsonl file")
        results = [triage_group(llm, g, trace) for g in group(load_reports(args.files)).values()]
        for r in [] if args.json else results:
            res = r["result"] or {"culprit_function": "-", "confidence": "-", "fix": "-",
                                  "root_cause": f"triage failed: {r['error']}"}
            print(f"== {r['signature']}  {r['type']}  x{r['count']}\n"
                  f"  culprit: {res['culprit_function']} ({res['confidence']} confidence)\n"
                  f"  cause:   {res['root_cause']}\n  fix:     {res['fix']}\n")
        print(json.dumps(results, indent=2) if args.json else "", end="")
        return 0 if all(r["result"] for r in results) else 1

    if not args.reports:
        ap.error("eval needs --reports DIR (make bugs keeps them in $BUILD/bug-reports)")
    summary = evaluate(llm, args.reports, json.loads(args.expected.read_text(encoding="utf-8")), trace)
    n, cols = summary["cases"], ("type", "culprit", "root_cause", "fix")
    lines = [f"{'bug':16} groups " + " ".join(f"{c:>10}" for c in cols)]
    lines += [f"{r['bug']:16} {r['groups']:>6} " + " ".join(f"{'yes' if r[c] else 'NO':>10}" for c in cols)
              for r in summary["rows"]]
    lines.append(f"\n{summary['llm']}: " + ", ".join(f"{c} {summary['totals'][c]}/{n} "
                                                    f"({100 * summary['totals'][c] // n}%)" for c in cols))
    print(json.dumps(summary, indent=2) if args.json else "\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
