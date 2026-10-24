#!/usr/bin/env python3
"""Read-only history paging through the real C++ CLI and disposable SQLite/files."""
from __future__ import annotations
import base64
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
FIXTURES = SOURCE / "tests/fixtures"
sys.argv = [sys.argv[0]]

class HistoryCli(unittest.TestCase):
    def setUp(self):
        old = os.umask(0o077)
        self.addCleanup(os.umask, old)
        tmp = tempfile.TemporaryDirectory(prefix="wls-r1-history-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.db = self.root / "control.sqlite"
        self.work = {node: self.root / node for node in ("alpha", "beta")}
        for path in self.work.values():
            path.mkdir(mode=0o700)
            (path / "r1.txt").write_text("原始内容😀\n" + "x" * 61100, encoding="utf-8")
        self.mapping = self.write("workspaces.json", {k: str(v) for k, v in self.work.items()})
        self.request = self.call("submit", "--db", self.db, "--key", "r1-history",
            "--text-file", FIXTURES / "s13_request.txt", "--constraints-file", FIXTURES / "s13_constraints.json")["request"]["request_id"]

    def write(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        return path

    def call(self, *args, error=None):
        result = subprocess.run([str(BINARY), *map(str, args), "--source-root", str(SOURCE)],
            cwd=self.root, capture_output=True, text=True, timeout=45, check=False)
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, "")
            self.assertEqual(json.loads(result.stderr)["error"], error, result.stderr)
            return json.loads(result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def rows(self, query):
        with closing(sqlite3.connect(self.db.as_uri() + "?mode=ro", uri=True)) as db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(query)]

    def snapshot(self):
        return {r["name"]: sorted(self.rows('SELECT * FROM "' + r["name"] + '"'), key=repr)
            for r in self.rows("SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%'")}

    def start(self):
        script = json.loads((FIXTURES / "s13_fake_initial.json").read_text())
        for steps in script["nodes"].values():
            for step in steps:
                if step["role"] == "worker":
                    step["tool_calls"] = [{"call_id": "read-original", "name": "fs_read",
                        "arguments": {"path": "r1.txt", "max_bytes": 65536}}]
        self.call("pump-fake", "--db", self.db, "--request", self.request,
            "--script", self.write("script.json", script), "--workspaces-file", self.mapping,
            "--worker-calls", 2, "--max-calls", 12)
        self.tasks = {t["node_id"]: t["task_id"] for t in self.call("show", "--db", self.db,
            "--request", self.request)["tasks"]}
        return self.page()["items"]

    def page(self, node="alpha", **options):
        args = ["history-page", "--db", self.db, "--request", self.request,
            "--role", "worker", "--task", self.tasks[node]]
        error = options.pop("error", None)
        for key, value in options.items(): args.extend(["--" + key, value])
        return self.call(*args, error=error)

    def read(self, item, node="alpha", section="result", offset=0, size=16384, error=None, hash_value=None):
        return self.call("history-read", "--db", self.db, "--request", self.request,
            "--role", "worker", "--task", self.tasks[node], "--attempt", item["attempt_id"],
            "--record-sha256", hash_value or item["record_sha256"], "--section", section,
            "--offset", offset, "--max-bytes", size, error=error)

    def test_empty_index_is_read_only(self):
        before = self.snapshot()
        result = self.call("history-page", "--db", self.db, "--request", self.request, "--role", "dispatcher")
        self.assertEqual(result["items"], [])
        self.assertIsNone(result["next_after_sequence"])
        self.assertEqual(self.snapshot(), before)

    def test_index_pages_and_reassembly_survive_workspace_change(self):
        original = self.start()
        self.assertGreaterEqual(len(original), 2)
        before = self.snapshot()
        first = self.page(limit=1)
        later = self.page(after=first["next_after_sequence"], through=first["through_sequence"], limit=64)
        self.assertEqual(first["items"] + later["items"], original)
        self.assertFalse(later["has_more"])
        (self.work["alpha"] / "r1.txt").write_text("changed current file", encoding="utf-8")
        item = original[0]
        for section in ("result", "response"):
            data, offset = b"", 0
            while True:
                page = self.read(item, section=section, offset=offset, size=7001)
                chunk = base64.b64decode(page["content_base64"], validate=True)
                self.assertEqual(len(chunk), page["bytes_returned"])
                self.assertLessEqual(len(chunk), 7001)
                data += chunk
                if page["eof"]:
                    self.assertIsNone(page["next_offset"])
                    break
                offset = page["next_offset"]
            self.assertEqual(hashlib.sha256(data).hexdigest(), page["section_sha256"])
            raw = self.rows("SELECT " + section + " FROM model_tool_feedback WHERE attempt_id='" + item["attempt_id"] + "'")[0][section]
            self.assertEqual(data.decode("utf-8"), raw)
            self.assertEqual(page["original_binding"], item["frozen_binding"])
        self.assertEqual(self.snapshot(), before)

    def test_sibling_cursor_and_record_are_rejected(self):
        own = self.start()[0]
        other = self.page("beta")["items"][0]
        before = self.snapshot()
        self.page(after=other["sequence"], error="unauthorized")
        self.page(through=other["sequence"], error="unauthorized")
        self.read(other, error="unauthorized")
        self.read(own, node="beta", error="unauthorized")
        self.assertEqual(self.snapshot(), before)

    def test_invalid_bounds_and_hashes_do_not_mutate(self):
        item = self.start()[0]
        before = self.snapshot()
        self.page(limit=65, error="invalid_input")
        self.page(after=-1, error="invalid_input")
        self.read(item, hash_value="0" * 64, error="conflict")
        self.read(item, section="request_envelope", error="invalid_input")
        self.read(item, offset=2**63-1, error="invalid_input")
        self.read(item, size=16385, error="invalid_input")
        self.assertEqual(self.snapshot(), before)

    def test_new_cli_preserves_legacy_complete_history(self):
        items = self.start()
        before = self.snapshot()
        legacy = self.call("history", "--db", self.db, "--request", self.request,
            "--role", "worker", "--task", self.tasks["alpha"])
        self.assertEqual([r["attempt_id"] for r in legacy], [r["attempt_id"] for r in items])
        self.assertNotIn("response", items[0])
        self.assertIn("response", legacy[0])
        self.assertEqual(self.snapshot(), before)

    def test_cross_request_task_is_not_a_read_capability(self):
        item = self.start()[0]
        other = self.call("submit", "--db", self.db, "--key", "r1-other-request",
            "--text-file", FIXTURES / "s13_request.txt", "--constraints-file", FIXTURES / "s13_constraints.json")["request"]["request_id"]
        before = self.snapshot()
        self.call("history-page", "--db", self.db, "--request", other, "--role", "worker",
            "--task", self.tasks["alpha"], error="unauthorized")
        self.call("history-read", "--db", self.db, "--request", other, "--role", "worker",
            "--task", self.tasks["alpha"], "--attempt", item["attempt_id"],
            "--record-sha256", item["record_sha256"], "--section", "result", error="unauthorized")
        self.assertEqual(self.snapshot(), before)

    def test_corrupt_feedback_is_not_returned(self):
        item = self.start()[0]
        with closing(sqlite3.connect(self.db)) as db, db:
            trigger = db.execute("SELECT sql FROM sqlite_schema WHERE name='model_tool_feedback_immutable'").fetchone()[0]
            db.execute("DROP TRIGGER model_tool_feedback_immutable")
            db.execute("UPDATE model_tool_feedback SET result='{}' WHERE attempt_id=?", (item["attempt_id"],))
            db.execute(trigger)  # Restore production protection before invoking the read guard.
        before = self.snapshot()
        self.read(item, error="corrupt")
        self.assertEqual(self.snapshot(), before)

if __name__ == "__main__": unittest.main()
