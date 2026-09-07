#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""A failed burst observation stays visible; a failed framed run never passes."""
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import verify_framing as verifier


class FramingEvidenceTests(unittest.TestCase):
    def setUp(self):
        source = Path(__file__).with_name("results_framing_2026-09-07.jsonl")
        self.rows = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines()]
        self.receipt = json.loads(Path(str(source) + ".session.json").read_text(encoding="utf-8"))

    def verify(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "record.jsonl"
            raw = ("\n".join(json.dumps(row) for row in self.rows) + "\n").encode()
            path.write_bytes(raw)
            receipt = copy.deepcopy(self.receipt)
            receipt["results_sha256"] = hashlib.sha256(raw).hexdigest()
            Path(str(path) + ".session.json").write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(verifier.Provenance, "check"), contextlib.redirect_stdout(io.StringIO()):
                return verifier.verify(path)

    def test_failed_burst_control_is_unavailable_not_passed(self):
        _, receipt, seen, exact, vectors = self.verify()
        self.assertTrue(exact and vectors)
        self.assertEqual(seen[(0, 10000000, "framing")]["status"], "failed")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            verifier.table(receipt, seen)
        row = next(line for line in output.getvalue().splitlines()
                   if line.startswith("| 10000000 | framing::None"))
        self.assertIn("unavailable:", row)
        self.assertNotIn("12/12", row)
        self.assertNotIn("0/12", row)

    def test_failed_framed_suite_is_rejected(self):
        row = next(r for r in self.rows if r["framer"] and r["suite"] == "framing")
        row.update(status="failed", error="injected framing failure")
        row.pop("summary")
        row.pop("trials")
        status = next(s for s in self.receipt["suites"]
                      if s["mode"] == 1 and s["baud"] == row["baud"] and s["suite"] == "framing")
        status["exit_code"] = 1
        with self.assertRaisesRegex(AssertionError, "framed framing suite failed"):
            self.verify()

    def test_failed_row_cannot_claim_a_successful_exit(self):
        status = next(s for s in self.receipt["suites"]
                      if s["mode"] == 0 and s["baud"] == 10000000 and s["suite"] == "framing")
        status["exit_code"] = 0
        with self.assertRaisesRegex(AssertionError, "record/exit status mismatch"):
            self.verify()

    def test_missing_burst_record_is_unavailable(self):
        self.rows = [r for r in self.rows
                     if not (not r["framer"] and r["baud"] == 10000000 and r["suite"] == "framing")]
        _, receipt, seen, exact, vectors = self.verify()
        self.assertTrue(exact and vectors)
        with contextlib.redirect_stdout(io.StringIO()):
            verifier.table(receipt, seen)
        self.assertIn("no HELLO", seen[(0, 10000000, "framing")]["error"])


if __name__ == "__main__":
    unittest.main()
