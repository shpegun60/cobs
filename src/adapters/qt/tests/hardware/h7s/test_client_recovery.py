#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Offline guards for live interop after the desktop client recovery fixes.

The artificial port failures/event orderings live in serial_adapter_test.
These records prove full reference-model interop and restoration on the H7S,
not physical injection of each of those host-only faults.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import unittest


HERE = Path(__file__).resolve().parent
CASES = tuple(HERE / f"results_qmodbus_2026-09-07_recovery{n}.json" for n in (1, 2))
DESKTOP_HEADERS = {"src/adapters/qt/RtuClient.h", "src/adapters/qt/SerialAdapter.h"}
BOOT_HASH = "a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456"


def read(path):
    return json.loads(path.read_bytes())


class ClientRecoveryTests(unittest.TestCase):
    def test_ordinary_verifier_accepts_both_full_rounds(self):
        for path in CASES:
            with self.subTest(record=path.name):
                result = subprocess.run([sys.executable, "-B", str(HERE / "verify_qmodbus.py"), str(path)],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS 8 runs", result.stdout)
                self.assertFalse(any(line.startswith("FAIL ") for line in result.stdout.splitlines()))

    def test_sessions_restore_the_original_boot_and_cover_every_configuration(self):
        expected = {(role, baud, framer) for role in ("server", "client")
                    for baud in (115200, 1000000) for framer in (0, 1)}
        for path in CASES:
            with self.subTest(record=path.name):
                record = read(path)
                receipt = read(Path(str(path) + ".session.json"))
                self.assertTrue(record["restored_and_verified"])
                self.assertTrue(receipt["completed"] and receipt["restored_and_verified"])
                self.assertEqual(receipt["backup_sha256"], BOOT_HASH)
                self.assertEqual(receipt["results_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
                self.assertEqual(record["runner_sha256"], receipt["runner_sha256"])
                self.assertEqual(record["port"], "COM6")
                self.assertEqual(receipt["serial"], "002A001F3033510135393935")
                self.assertEqual(len(record["runs"]), 8)
                self.assertEqual({(r["role"], r["baud"], r["framer"]) for r in record["runs"]}, expected)
                self.assertEqual([r["image"] for r in record["runs"]], receipt["images"])
                self.assertEqual((record["timeout_ms"], record["retries"], record["client_delay_ms"]),
                                 (1000, 0, 2500))
                self.assertEqual(record["server_inter_frame_us"], 50000)
                self.assertTrue(record["server_trace"])
                self.assertEqual(record["stall_first_read_fragment_ms"], 0)

    def test_only_the_two_desktop_production_headers_changed(self):
        baseline = read(HERE / "results_qmodbus_2026-09-07_usb_defaults.json")["source_sha256"]
        sources = []
        for path in CASES:
            with self.subTest(record=path.name):
                current = read(path)["source_sha256"]
                self.assertEqual(current.keys(), baseline.keys())
                changed = {name for name in current if current[name] != baseline[name]}
                self.assertEqual(changed, DESKTOP_HEADERS)
                sources.append(current)
        self.assertEqual(sources[0], sources[1])

    def test_every_flashed_mcu_image_is_byte_identical_to_the_previous_control(self):
        baseline = read(HERE / "results_qmodbus_2026-09-07_usb_repeat3.json")
        images = {(r["role"], r["baud"], r["framer"]): r["image"] for r in baseline["runs"]}
        for path in CASES:
            for run in read(path)["runs"]:
                with self.subTest(record=path.name, role=run["role"], baud=run["baud"], framer=run["framer"]):
                    old = images[(run["role"], run["baud"], run["framer"])]
                    self.assertEqual(run["image"]["binary_sha256"], old["binary_sha256"])
                    self.assertEqual(run["image"]["elf_sha256"], old["elf_sha256"])

    def test_complete_scenario_indices_and_no_new_board_errors(self):
        total = 0
        for path in CASES:
            for run in read(path)["runs"]:
                with self.subTest(record=path.name, role=run["role"], baud=run["baud"], framer=run["framer"]):
                    scripts = ([run["board"]["entries"]] if run["role"] == "client" else
                               [run[name]["scenarios"] for name in ("qtclient", "ourclient")])
                    for steps in scripts:
                        self.assertEqual([step["index"] for step in steps], list(range(55)))
                        total += len(steps)
                    if run["role"] == "client":
                        self.assertEqual([e["index"] for e in run["board"]["entries"] if e["status"] != "ok"], [19])
                        for counter in ("uart_rx_errors", "uart_tx_errors", "uart_rx_overrun",
                                        "uart_restarts", "rtu_crc_errors"):
                            self.assertEqual(run["board_stats"][counter], 0, counter)
        self.assertEqual(total, 1320)


if __name__ == "__main__":
    unittest.main()
