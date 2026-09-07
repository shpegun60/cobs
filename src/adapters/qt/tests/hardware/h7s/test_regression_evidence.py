#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Keep the two observed Qt failures visible alongside the passing repeats.

This is an offline evidence regression, NOT a passing Qt interoperability
claim. The ordinary verifier must still return failure for both failed runs.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest


HERE = Path(__file__).resolve().parent
PREFIX = "results_qmodbus_2026-09-07"
CASES = (("", 8, 1, "step 0 timeout"),
         ("_repeat1", 2, 0, "PASS 2 runs"),
         ("_repeat2_full", 8, 1, "step 1 timeout"),
         ("_repeat3_control", 8, 0, "PASS 8 runs"))


class RegressionEvidenceTests(unittest.TestCase):
    def test_every_session_is_complete_and_bound_to_its_images(self):
        backups = set()
        for suffix, count, _, _ in CASES:
            with self.subTest(record=suffix or "first"):
                path = HERE / (PREFIX + suffix + ".json")
                raw = path.read_bytes()
                record = json.loads(raw)
                receipt = json.loads(Path(str(path) + ".session.json").read_bytes())
                self.assertTrue(receipt["completed"] and receipt["restored_and_verified"])
                self.assertEqual(hashlib.sha256(raw).hexdigest(), receipt["results_sha256"])
                self.assertEqual(record["runner_sha256"], receipt["runner_sha256"])
                self.assertEqual(record["retries"], 0)
                self.assertEqual(record["timeout_ms"], 1000)
                self.assertEqual(record["client_delay_ms"], 2500)
                self.assertEqual(record["server_seconds"], 10)
                self.assertEqual(len(record["runs"]), count)
                expected = {(role, baud, framer) for role in ("server", "client")
                            for baud in record["bauds"] for framer in record["framers"]}
                self.assertEqual({(r["role"], r["baud"], r["framer"]) for r in record["runs"]}, expected)
                self.assertEqual([r["image"] for r in record["runs"]], receipt["images"])
                backups.add(receipt["backup_sha256"])
                for run in record["runs"]:
                    image = run["image"]
                    self.assertEqual(image["baud"], run["baud"])
                    self.assertEqual(image["framer"], run["framer"])
                    scenarios = ([run["board"]["entries"]] if run["role"] == "client" else
                                 [run[c]["scenarios"] for c in ("qtclient", "ourclient")])
                    for steps in scenarios:
                        self.assertEqual([s["index"] for s in steps], list(range(55)))
        self.assertEqual(backups, {"a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456"})

    def test_verifier_still_rejects_both_observed_failures(self):
        for suffix, _, code, message in CASES:
            with self.subTest(record=suffix or "first"):
                result = subprocess.run([sys.executable, "-B", str(HERE / "verify_qmodbus.py"),
                                         str(HERE / (PREFIX + suffix + ".json"))],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertIn(message, result.stdout)
                if code:
                    self.assertEqual(sum(line.startswith("FAIL ") for line in result.stdout.splitlines()), 1)

    def test_trace_is_bound_to_the_diagnostic_run(self):
        trace = json.loads((HERE / (PREFIX + "_repeat2_full.trace.json")).read_bytes())
        raw = (HERE / trace["record"]).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), trace["results_sha256"])
        self.assertEqual(trace["first_line"], 1)
        self.assertEqual(len(trace["lines"]), 13)
        record = json.loads(raw)
        run = next(r for r in record["runs"] if r["role"] == "client" and r["baud"] == 115200 and not r["framer"])
        self.assertEqual(run["board"]["entries"][1]["status"], "timeout")
        text = "\n".join(trace["lines"])
        self.assertIn('Received ADU: "0a"', text)
        self.assertIn("Dropping older ADU fragments", text)
        self.assertIn('Received ADU: "040000000a7176"', text)
        # No hardware error was reported by the board for this host-side drop.
        for counter in ("uart_rx_errors", "uart_tx_errors", "uart_rx_overrun", "uart_restarts", "rtu_crc_errors"):
            self.assertEqual(run["board_stats"][counter], 0, counter)


if __name__ == "__main__":
    unittest.main()
