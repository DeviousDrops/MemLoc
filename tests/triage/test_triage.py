#!/usr/bin/env python3
"""Offline tests for tools/triage.py (D-39). No network and no API key.

The reports in tests/triage/reports are real `make bugs` output with the
build and source paths rewritten, so they also exercise path re-rooting.
"""
from __future__ import annotations

import copy
import http.server
import importlib.util
import json
import sys
import tempfile
import threading
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import triage  # noqa: E402

REPORTS = HERE / "reports"
EXPECTED = json.loads((ROOT / "tests" / "bugs" / "expected.json").read_text(encoding="utf-8"))
GOOD = {"bug_type": "heap-buffer-overflow", "culprit_function": "copy_name",
        "root_cause": "strcpy writes the NUL one byte past a strlen(name) block.",
        "fix": "Allocate strlen(name) + 1 bytes.", "confidence": "high"}


def first_group(name: str) -> dict:
    return next(iter(triage.group(triage.load_reports([REPORTS / f"{name}.jsonl"])).values()))


class FakeClient:
    """Stands in for anthropic.Anthropic(): records the request, returns a canned message."""

    def __init__(self, text: str, stop_reason: str = "end_turn"):
        self.calls: list[dict] = []
        content = [types.SimpleNamespace(type="thinking", thinking=""),
                   types.SimpleNamespace(type="text", text=text)]
        reply = types.SimpleNamespace(model="claude-opus-5", stop_reason=stop_reason, content=content,
                                      usage=types.SimpleNamespace(input_tokens=900, output_tokens=120))
        create = lambda **kw: self.calls.append(kw) or reply
        self.beta = types.SimpleNamespace(messages=types.SimpleNamespace(create=create))


def claude(client) -> "triage.ClaudeLLM":
    """A ClaudeLLM on a fake client. The anthropic package is optional in these tests."""
    llm = triage.ClaudeLLM.__new__(triage.ClaudeLLM)
    llm.model, llm.client = triage.MODEL, client
    llm.anthropic = types.SimpleNamespace(AuthenticationError=type("A", (Exception,), {}),
                                          APIError=type("E", (Exception,), {}))
    return llm


class Signatures(unittest.TestCase):
    def test_ignores_addresses_pids_offsets_and_lines(self):
        rec = first_group("double_free")["report"]
        other = copy.deepcopy(rec)
        other.update(pid=1, tid=2, address="0x1")
        for f in other["stack"] + other["alloc_stack"]:
            f.update(pc="0xdead", offset="0x9", line=(f.get("line") or 0) + 40)
        self.assertEqual(triage.signature(rec), triage.signature(other))

    def test_changes_with_a_user_function_or_the_type(self):
        rec = first_group("double_free")["report"]
        renamed = copy.deepcopy(rec)
        triage.user_frames(renamed["stack"])[0]["function"] = "somewhere_else"
        retyped = dict(rec, type="invalid-free")
        self.assertEqual(len({triage.signature(r) for r in (rec, renamed, retyped)}), 3)

    def test_user_frames_skip_libc_loader_and_start(self):
        rec = first_group("new_overflow")["report"]
        names = [triage.frame_name(f) for f in triage.user_frames(rec["alloc_stack"])]
        self.assertEqual(names[0], "make_squares")
        self.assertNotIn("_start", names)
        self.assertTrue(all("libc" not in f["module"] for f in triage.user_frames(rec["stack"])))


class Grouping(unittest.TestCase):
    def test_duplicates_merge_and_leak_summary_is_dropped(self):
        path = REPORTS / "leak.jsonl"
        groups = triage.group(triage.load_reports([path, path, path]))
        self.assertEqual(len(groups), 2)  # two leak sites; the summary line is gone
        self.assertEqual({g["count"] for g in groups.values()}, {3})
        self.assertEqual({g["report"]["type"] for g in groups.values()}, {"leak"})

    def test_prompt_has_report_and_source_from_the_checkout(self):
        prompt = triage.build_prompt(first_group("off_by_one"))
        self.assertIn("heap-buffer-overflow", prompt)
        self.assertIn("copy_name", prompt)
        self.assertIn("strcpy", prompt)  # off_by_one.c, found by re-rooting /src/memloc/...
        self.assertIsNone(triage.resolve("/nowhere/at/all.c"))


class MockBaseline(unittest.TestCase):
    def test_eval_scores(self):
        with tempfile.TemporaryDirectory() as d:
            trace = triage.Trace(Path(d) / "t.jsonl")
            summary = triage.evaluate(triage.MockLLM(), REPORTS, EXPECTED, trace)
            entries = [json.loads(ln) for ln in trace.path.read_text().splitlines()]
        self.assertEqual(summary["cases"], 11)
        self.assertEqual(summary["totals"]["type"], 11)
        self.assertEqual(summary["totals"]["culprit"], 11)
        self.assertLess(summary["totals"]["fix"], 11)  # the baseline cannot explain a fix
        self.assertEqual(sum(e["kind"] == "triage" for e in entries), 12)  # leak has 2 groups
        self.assertEqual(sum(e["kind"] == "judge" for e in entries), 11)
        self.assertTrue(all(e["prompt"] and e["error"] is None for e in entries))

    def test_missing_report_scores_zero(self):
        with tempfile.TemporaryDirectory() as d:
            summary = triage.evaluate(triage.MockLLM(), Path(d), EXPECTED, triage.Trace(None))
        self.assertEqual(summary["totals"], {"type": 0, "culprit": 0, "root_cause": 0, "fix": 0})


class ClaudePath(unittest.TestCase):
    def test_request_shape_and_parsing(self):
        client = FakeClient(json.dumps(GOOD))
        with tempfile.TemporaryDirectory() as d:
            trace = triage.Trace(Path(d) / "t.jsonl")
            out = triage.triage_group(claude(client), first_group("off_by_one"), trace)
            entry = json.loads(trace.path.read_text())
        self.assertEqual(out["result"], GOOD)
        kw = client.calls[0]
        self.assertEqual(kw["model"], "claude-opus-5")
        self.assertEqual(kw["thinking"], {"type": "adaptive"})
        self.assertEqual(kw["output_config"]["format"]["schema"], triage.RESULT_SCHEMA)
        self.assertEqual((kw["fallbacks"], kw["betas"]), ("default", ["server-side-fallback-2026-07-01"]))
        self.assertIn("copy_name", kw["messages"][0]["content"])
        self.assertEqual(entry["meta"]["usage"], {"input_tokens": 900, "output_tokens": 120})
        self.assertEqual(entry["signature"], out["signature"])

    def test_refusal_is_recorded_not_raised(self):
        out = triage.triage_group(claude(FakeClient("", "refusal")), first_group("leak"), triage.Trace(None))
        self.assertIsNone(out["result"])
        self.assertIn("refusal", out["error"])

    def test_invalid_fields_are_rejected(self):
        bad = dict(GOOD, confidence="certain")
        out = triage.triage_group(claude(FakeClient(json.dumps(bad))), first_group("off_by_one"),
                                  triage.Trace(None))
        self.assertIn("enum", out["error"])

    def test_api_errors_become_group_failures(self):
        llm = claude(FakeClient(""))
        llm.client.beta.messages.create = lambda **kw: (_ for _ in ()).throw(llm.anthropic.APIError("503"))
        out = triage.triage_group(llm, first_group("off_by_one"), triage.Trace(None))
        self.assertIn("503", out["error"])


@unittest.skipUnless(importlib.util.find_spec("anthropic"), "anthropic package not installed")
class RealSdkAgainstStub(unittest.TestCase):
    """The real SDK against a local HTTP stub: checks the wire format, still offline."""

    def test_wire_request(self):
        import anthropic
        seen = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                seen["path"], seen["beta"] = self.path, self.headers.get("anthropic-beta")
                seen["body"] = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                body = json.dumps({"id": "msg_1", "type": "message", "role": "assistant",
                                   "model": "claude-opus-5", "stop_reason": "end_turn",
                                   "stop_sequence": None, "usage": {"input_tokens": 5, "output_tokens": 7},
                                   "content": [{"type": "text", "text": json.dumps(GOOD)}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            client = anthropic.Anthropic(api_key="test", max_retries=0,
                                         base_url=f"http://127.0.0.1:{server.server_port}")
            result, meta = triage.ClaudeLLM(client=client).triage("prompt", {})
            client.close()
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual(result, GOOD)
        self.assertEqual(meta["usage"], {"input_tokens": 5, "output_tokens": 7})
        self.assertTrue(seen["path"].startswith("/v1/messages"))
        self.assertIn("server-side-fallback-2026-07-01", seen["beta"])
        self.assertEqual(seen["body"]["fallbacks"], "default")
        self.assertEqual(seen["body"]["output_config"]["format"]["type"], "json_schema")
        self.assertEqual(seen["body"]["thinking"], {"type": "adaptive"})


if __name__ == "__main__":
    unittest.main()
