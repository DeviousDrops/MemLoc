#!/usr/bin/env python3
"""Offline tests for tools/triage.py (D-39). No network and no API key.

The Groq transport is faked or pointed at a local stub server.
The reports in tests/triage/reports are real `make bugs` output with the
build and source paths rewritten, so they also exercise path re-rooting.
"""
from __future__ import annotations

import copy
import http.server
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

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


def reply(content: str, finish: str = "stop") -> dict:
    return {"model": triage.MODEL, "usage": {"prompt_tokens": 900, "completion_tokens": 120},
            "choices": [{"finish_reason": finish, "message": {"role": "assistant", "content": content}}]}


class FakePost:
    """Stands in for the HTTP transport: records each request body, replays canned responses."""

    def __init__(self, *responses: tuple[int, dict, dict]):
        self.responses, self.bodies = list(responses), []

    def __call__(self, body: dict) -> tuple[int, dict, dict]:
        self.bodies.append(body)
        return self.responses.pop(0)


def groq(*responses) -> "triage.GroqLLM":
    return triage.GroqLLM(post=FakePost(*responses))


class GroqPath(unittest.TestCase):
    def test_request_shape_and_parsing(self):
        llm = groq((200, {}, reply(json.dumps(GOOD))))
        with tempfile.TemporaryDirectory() as d:
            trace = triage.Trace(Path(d) / "t.jsonl")
            out = triage.triage_group(llm, first_group("off_by_one"), trace)
            entry = json.loads(trace.path.read_text())
        self.assertEqual(out["result"], GOOD)
        body = llm.post.bodies[0]
        self.assertEqual(body["model"], "openai/gpt-oss-120b")
        fmt = body["response_format"]
        self.assertEqual((fmt["type"], fmt["json_schema"]["strict"]), ("json_schema", True))
        self.assertEqual(fmt["json_schema"]["schema"], triage.RESULT_SCHEMA)
        self.assertEqual([m["role"] for m in body["messages"]], ["system", "user"])
        self.assertIn("copy_name", body["messages"][1]["content"])
        self.assertEqual(entry["meta"]["usage"], {"prompt_tokens": 900, "completion_tokens": 120})
        self.assertEqual(entry["signature"], out["signature"])

    def test_rate_limit_is_retried_after_the_advertised_delay(self):
        llm = groq((429, {"retry-after": "0"}, {"error": {"message": "slow down"}}),
                   (200, {}, reply(json.dumps(GOOD))))
        with tempfile.TemporaryDirectory() as d:
            trace = triage.Trace(Path(d) / "t.jsonl")
            out = triage.triage_group(llm, first_group("off_by_one"), trace)
            entry = json.loads(trace.path.read_text())
        self.assertEqual(out["result"], GOOD)
        self.assertEqual((len(llm.post.bodies), entry["meta"]["attempts"]), (2, 2))

    def test_truncated_output_is_recorded_not_raised(self):
        out = triage.triage_group(groq((200, {}, reply('{"bug_type": "le', "length"))),
                                  first_group("leak"), triage.Trace(None))
        self.assertIsNone(out["result"])
        self.assertIn("length", out["error"])

    def test_invalid_fields_are_rejected(self):
        bad = dict(GOOD, confidence="certain")
        out = triage.triage_group(groq((200, {}, reply(json.dumps(bad)))), first_group("off_by_one"),
                                  triage.Trace(None))
        self.assertIn("enum", out["error"])

    def test_http_errors_become_group_failures(self):
        out = triage.triage_group(groq((400, {}, {"error": {"message": "does not match the schema"}})),
                                  first_group("off_by_one"), triage.Trace(None))
        self.assertIn("HTTP 400: does not match the schema", out["error"])

    def test_bad_key_stops_the_run(self):
        with self.assertRaises(SystemExit):
            triage.triage_group(groq((401, {}, {"error": {"message": "invalid key"}})),
                                first_group("off_by_one"), triage.Trace(None))


class RealClientAgainstStub(unittest.TestCase):
    """The real urllib transport against a local HTTP stub: the wire format, still offline."""

    def test_wire_request(self):
        seen = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                seen["path"], seen["auth"] = self.path, self.headers.get("Authorization")
                seen["body"] = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                body = json.dumps(reply(json.dumps(GOOD))).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{server.server_port}/openai/v1/chat/completions"
        try:
            with mock.patch.dict(os.environ, {"GROQ_API_KEY": "test-key"}):
                result, meta = triage.GroqLLM(url=url).triage("prompt", {})
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual(result, GOOD)
        self.assertEqual(meta["attempts"], 1)
        self.assertEqual(seen["path"], "/openai/v1/chat/completions")
        self.assertEqual(seen["auth"], "Bearer test-key")
        self.assertEqual(seen["body"]["response_format"]["json_schema"]["name"], "triage")


if __name__ == "__main__":
    unittest.main()
