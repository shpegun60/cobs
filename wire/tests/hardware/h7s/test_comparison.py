#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
import unittest
import run_comparison as bench


class ComparisonTests(unittest.TestCase):
    def test_deterministic_vectors(self):
        self.assertEqual(bench.payload(8, 0).hex(), "12ea6abf133bafef")
        self.assertEqual(bench.payload(8, 1), bytes(8))
        self.assertEqual(bench.payload(8, 2), bytes(range(1, 9)))
        self.assertEqual(bench.payload(8, 3).hex(), "00a500a500a500a5")

    def test_independent_wire_oracles(self):
        for size in bench.SIZES:
            for pattern in range(5):
                body = bench.payload(size, pattern)
                for policy in bench.POLICIES:
                    maximum = 1024 if size == 1024 else (255 if policy == "none" else 253)
                    encoded = bench.wire("cobs", policy, body, maximum)
                    raw = bench.cobs.cobs_decode(encoded[:-1])
                    width = 2 if size == 1024 else 1
                    trailer = 0 if policy == "none" else 2
                    self.assertEqual(int.from_bytes(raw[:width], "little"), size + trailer)
                    self.assertEqual(raw[width:width + size], body)
                    self.assertTrue(bench.policy_for(policy).verify(raw[width:]))
                    adu = bench.wire("rtu", policy, body)
                    self.assertEqual(adu[:2], b"\x11\x41")
                    self.assertEqual(adu[2:2 + size], body)
                    self.assertTrue(bench.policy_for(policy).verify(adu))
                    self.assertEqual(len(adu), size + 2 + trailer)

    def test_table_does_not_change_wire(self):
        for protocol in ("cobs", "rtu"):
            for size in bench.SIZES:
                body = bench.payload(size, 0)
                self.assertEqual(bench.wire(protocol, "bitwise", body, 1024),
                                 bench.wire(protocol, "table", body, 1024))

    def test_cadence_budget_for_worst_frame(self):
        for case in bench.CASES:
            for baud in (115200, 1000000):
                fps = bench.target_fps(case, baud)
                self.assertTrue(1 <= fps <= 300)
                for body in bench.corpus(case):
                    for protocol in ("cobs", "rtu"):
                        for policy in bench.POLICIES:
                            self.assertLessEqual(len(bench.wire(protocol, policy, body)) * 20 * fps / baud, 0.75)


if __name__ == "__main__":
    unittest.main()
