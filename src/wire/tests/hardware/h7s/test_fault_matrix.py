#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Offline negative tests of the live-matrix evidence; never opens a port."""
import contextlib
import copy
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import verify_fault_matrix as verifier


HERE = Path(__file__).resolve().parent
RECORD = HERE / "results_fault_matrix_2026-09-07"


class FaultMatrixTests(unittest.TestCase):
    def setUp(self):
        self.receipt = json.loads((RECORD / "session.json").read_text(encoding="utf-8"))

    def row(self, tag, suite):
        phase = next(p for p in verifier.plan() if p["tag"] == tag)
        rows = [json.loads(line) for line in (RECORD / (tag + ".jsonl")).read_text(encoding="utf-8").splitlines()]
        return phase, next(row for row in rows if row["suite"] == suite)

    def check_receipt(self, edit=None):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "evidence"
            shutil.copytree(RECORD, folder)
            (folder / "session.json").write_text(json.dumps(self.receipt), encoding="utf-8")
            if edit:
                edit(folder)
            # Provenance is checked separately against Git, not against these
            # deliberately edited fixtures. No hardware or local ELF is used.
            with patch.object(verifier.Provenance, "check"), contextlib.redirect_stdout(io.StringIO()):
                return verifier.verify(folder)

    def test_full_record_and_plan(self):
        phases = verifier.plan()
        self.assertEqual(len(phases), 33)
        self.assertEqual(len({p["tag"] for p in phases}), 33)
        self.assertEqual(self.check_receipt(), {"cobs": 93, "rtu": 144})

    def test_incomplete_session_is_rejected(self):
        self.receipt["completed"] = False
        with self.assertRaises(AssertionError):
            self.check_receipt()

    def test_unverified_restore_is_rejected(self):
        self.receipt["restored_and_verified"] = False
        with self.assertRaises(AssertionError):
            self.check_receipt()

    def test_changed_backup_is_rejected(self):
        self.receipt["readback_sha256"] = "0" * 64
        with self.assertRaises(AssertionError):
            self.check_receipt()

    def test_narrowed_plan_is_rejected(self):
        self.receipt["plan"].pop()
        self.receipt["phases"].pop()
        with self.assertRaisesRegex(AssertionError, "must not be narrowed"):
            self.check_receipt()

    def test_missing_phase_is_rejected(self):
        self.receipt["phases"].pop()
        with self.assertRaisesRegex(AssertionError, "missing/reordered"):
            self.check_receipt()

    def test_child_failure_is_rejected(self):
        self.receipt["phases"][0]["exit_code"] = 1
        with self.assertRaises(AssertionError):
            self.check_receipt()

    def test_changed_raw_bytes_are_rejected(self):
        def edit(folder):
            path = folder / self.receipt["phases"][0]["results"]
            path.write_bytes(path.read_bytes() + b"\n")
        with self.assertRaisesRegex(AssertionError, "altered result"):
            self.check_receipt(edit)

    def test_cobs_wrong_crc_count_is_rejected(self):
        phase, row = self.row("cobs-bitwise-253-115200", "faults")
        verifier.check_cobs(phase, row)
        row["stats"]["cobs_crc_errors"] = 0
        with self.assertRaises(AssertionError):
            verifier.check_cobs(phase, row)

    def test_cobs_gap_requires_physical_overrun(self):
        phase, row = self.row("cobs-none-1024-10000000", "gap")
        verifier.check_cobs(phase, row)
        row["stats"]["uart_rx_overrun"] = 0
        with self.assertRaises(AssertionError):
            verifier.check_cobs(phase, row)

    def test_cobs_pool_requires_complete_release(self):
        phase, row = self.row("cobs-table-253-1000000", "pool")
        row["stats"]["rx_in_use"] += 1
        with self.assertRaises(AssertionError):
            verifier.check_cobs(phase, row)

    def test_nocrc_acceptance_cannot_be_claimed_as_detection(self):
        phase, row = self.row("rtu-nocrc-115200", "faults")
        verifier.check_rtu(phase, row)
        row["stats"]["rtu_crc_errors"] = 4
        with self.assertRaises(AssertionError):
            verifier.check_rtu(phase, row)

    def test_rtu_image_cannot_be_relabelled(self):
        phase, row = self.row("rtu-crc16-table-115200", "vectors")
        for field, value in (("policy", "crc16-bitwise"), ("baud", 1000000), ("framer", True)):
            with self.subTest(field=field):
                changed = copy.deepcopy(row)
                changed["image"][field] = value
                with self.assertRaises(AssertionError):
                    verifier.check_rtu(phase, changed)

    def test_unused_table_must_be_absent(self):
        phase, row = self.row("rtu-crc32-bitwise-115200", "vectors")
        row["image"]["lookup_bytes"] = 1024
        with self.assertRaises(AssertionError):
            verifier.check_rtu(phase, row)

    def test_crc_benchmark_checks_independent_oracle(self):
        phase, row = self.row("rtu-crc64-table-115200", "crc_benchmark")
        verifier.check_rtu(phase, row)
        row["measurements"][-1]["samples"][0]["checksum"] ^= 1
        with self.assertRaises(AssertionError):
            verifier.check_rtu(phase, row)

    def test_cpu_formula_cannot_be_relabelled(self):
        phase, row = self.row("rtu-crc16-bitwise-1000000", "paced")
        verifier.check_rtu(phase, row)
        row["integrated_cpu_percent"] *= 2
        with self.assertRaises(AssertionError):
            verifier.check_rtu(phase, row)


if __name__ == "__main__":
    unittest.main()
