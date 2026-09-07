#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Offline guards for the native-vs-USB Qt fragment-deadline experiment."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest

import verify_qmodbus as verifier


HERE = Path(__file__).resolve().parent
PREFIX = "results_qmodbus_2026-09-07_"
CASES = (("native_stall", 2, 1), ("usb_stall", 4, 0),
         ("usb_repeat1", 8, 0), ("usb_repeat2", 8, 0), ("usb_repeat3", 8, 0),
         ("usb_defaults", 2, 0))


def read(tag):
    return json.loads((HERE / (PREFIX + tag + ".json")).read_bytes())


class UsbDeadlineTests(unittest.TestCase):
    def setUp(self):
        self.record = read("usb_stall")
        self.run = next(r for r in self.record["runs"] if r["role"] == "client")

    def errors(self):
        errors = []
        verifier.check_server_trace(self.record, self.run,
                                    lambda condition, message: None if condition else errors.append(message))
        return errors

    def test_all_record_verdicts_remain_visible(self):
        for tag, count, expected_exit in CASES:
            with self.subTest(tag=tag):
                path = HERE / (PREFIX + tag + ".json")
                record = json.loads(path.read_bytes())
                self.assertEqual(len(record["runs"]), count)
                result = subprocess.run([sys.executable, "-B", str(HERE / "verify_qmodbus.py"), str(path)],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
                if expected_exit:
                    self.assertEqual(sum(line.startswith("FAIL ") for line in result.stdout.splitlines()), 1)
                    self.assertIn("step 0 timeout", result.stdout)

    def test_same_firmware_no_retries_and_verified_restoration(self):
        baseline = json.loads((HERE / "results_qmodbus_2026-09-07_repeat3_control.json").read_bytes())
        binaries = {(r["role"], r["baud"], r["framer"]): r["image"]["binary_sha256"] for r in baseline["runs"]}
        elfs = {(r["role"], r["baud"], r["framer"]): r["image"]["elf_sha256"] for r in baseline["runs"]}
        for tag, _, _ in CASES:
            with self.subTest(tag=tag):
                path = HERE / (PREFIX + tag + ".json")
                raw = path.read_bytes()
                record = json.loads(raw)
                receipt = json.loads(Path(str(path) + ".session.json").read_bytes())
                self.assertTrue(receipt["completed"] and receipt["restored_and_verified"])
                self.assertEqual(receipt["results_sha256"], hashlib.sha256(raw).hexdigest())
                self.assertEqual(receipt["backup_sha256"], "a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456")
                self.assertEqual((record["timeout_ms"], record["retries"], record["client_delay_ms"]), (1000, 0, 2500))
                for run in record["runs"]:
                    self.assertEqual(run["image"]["binary_sha256"], binaries[(run["role"], run["baud"], run["framer"])])
                    self.assertEqual(run["image"]["elf_sha256"], elfs[(run["role"], run["baud"], run["framer"])])

    def test_normal_runner_uses_usb_deadline_and_keeps_trace(self):
        record = read("usb_defaults")
        self.assertEqual(record["server_inter_frame_us"], 50000)
        self.assertTrue(record["server_trace"])
        self.assertEqual(record["stall_first_read_fragment_ms"], 0)

    def test_native_stall_reproduces_the_first_request_loss(self):
        native = read("native_stall")
        run = next(r for r in native["runs"] if r["role"] == "client")
        self.assertEqual(run["qtserver"]["inter_frame_delay_us"], 2000)
        self.assertEqual(run["board"]["entries"][0]["status"], "timeout")
        trace = run["qtserver"]["trace"]
        self.assertTrue(trace["stall_injected"])
        self.assertGreaterEqual(trace["stall_elapsed_us"], 10000)
        self.assertIn("Dropping older ADU fragments", trace["entries"][2]["message"])
        for name in ("uart_rx_errors", "uart_tx_errors", "uart_rx_overrun", "uart_restarts", "rtu_crc_errors"):
            self.assertEqual(run["board_stats"][name], 0)

    def test_usb_deadline_passes_the_same_injected_stall(self):
        for run in self.record["runs"]:
            if run["role"] == "client":
                self.assertEqual(run["qtserver"]["inter_frame_delay_us"], 50000)
                self.assertTrue(run["qtserver"]["trace"]["stall_injected"])
                self.assertEqual(run["board"]["entries"][0]["status"], "ok")
                self.assertEqual([e["index"] for e in run["board"]["entries"] if e["status"] != "ok"], [19])
        self.assertEqual(self.errors(), [])

    def test_deadline_cannot_be_relabelled(self):
        self.run["qtserver"]["inter_frame_delay_us"] = 2000
        self.assertTrue(any("deadline" in error for error in self.errors()))

    def test_injection_must_actually_happen(self):
        self.run["qtserver"]["trace"]["stall_injected"] = False
        self.assertTrue(any("not injected" in error for error in self.errors()))

    def test_injection_duration_is_checked(self):
        self.run["qtserver"]["trace"]["stall_elapsed_us"] = 100
        self.assertTrue(any("shorter" in error for error in self.errors()))

    def test_trace_overflow_is_not_silently_accepted(self):
        self.run["qtserver"]["trace"]["overflow"] = True
        self.assertTrue(any("overflow" in error for error in self.errors()))

    def test_trace_must_be_present_and_ordered(self):
        original = copy.deepcopy(self.run["qtserver"]["trace"])
        self.run["qtserver"]["trace"]["entries"] = []
        self.assertTrue(any("empty" in error for error in self.errors()))
        self.run["qtserver"]["trace"] = original
        self.run["qtserver"]["trace"]["entries"][1]["at_us"] = -1
        self.assertTrue(any("monotonic" in error for error in self.errors()))


if __name__ == "__main__":
    unittest.main()
