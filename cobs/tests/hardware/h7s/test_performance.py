#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Offline contracts for the live performance runner (no board needed)."""
from collections import deque
import unittest

import cobs_performance as perf
import verify_performance as audit


class Port:
    def __init__(self, chunks):
        self.chunks = deque(chunks)

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, _count):
        return self.chunks.popleft() if self.chunks else b""


class PerformanceTests(unittest.TestCase):
    def test_same_payload_corpus_across_policies(self):
        count = 0
        for maximum in (253, 1024):
            for case in perf.cases(maximum):
                variants = []
                for policy in perf.POLICIES:
                    perf.configure(policy, maximum)
                    bodies, encoded, digest = perf.corpus(case)
                    variants.append((bodies, encoded, digest))
                    self.assertTrue(all(len(body) <= maximum for body in bodies))
                    self.assertEqual(perf.peer.LENGTH_SIZE, 1 if maximum == 253 else 2)
                    self.assertEqual(perf.peer.CRC_SIZE, 0 if policy == "none" else 2)
                    count += len(bodies)
                self.assertEqual(variants[0][0], variants[1][0])
                self.assertEqual(variants[0][2], variants[2][2])
                self.assertEqual(variants[1], variants[2])  # Table changes no wire bytes.
                self.assertNotEqual(variants[0][1], variants[1][1])
        self.assertEqual(count, 720)

    def test_nested_counters_not_double_added(self):
        counters = {name: {"total": 100} for name in (*perf.IRQ_COUNTERS, *perf.THREAD_COUNTERS)}
        counters.update(cobs_consume={"total": 999999}, uart_rx={"total": 999999},
                        uart_tx_start={"total": 999999})
        values = perf.metrics({"window_ms": 1000, "counters": counters},
                              600000000, 1000000, 10, 320, 360)
        self.assertEqual(values["instrumented_cycles"], 600)
        self.assertEqual(values["cycles_per_echo"], 60)
        self.assertAlmostEqual(values["instrumented_cpu_percent"], 0.0001)
        self.assertAlmostEqual(values["wire_utilization_percent_per_direction"], 0.36)
        self.assertAlmostEqual(values["extrapolated_full_line_cpu_percent"],
                               values["instrumented_cpu_percent"] * 100 / 0.36)
        self.assertAlmostEqual(values["scope_union_lower_percent"], 0.00005)

    def test_invalid_denominator(self):
        with self.assertRaises(ValueError):
            perf.metrics({"window_ms": 0}, 600000000, 1000000, 1, 1, 1)

    def test_wire_reader_fragmentation_and_coalescing(self):
        reader = perf.WireReader(Port([b"\x02", b"\x11\0\x02\x22\0\x03", b"\x33\x44\0"]))
        self.assertEqual(reader.read(), b"\x02\x11\0")
        self.assertEqual(reader.read(), b"\x02\x22\0")
        self.assertEqual(reader.read(), b"\x03\x33\x44\0")
        self.assertFalse(reader.partial or reader.frames)

    def test_extra_delimiter_not_silently_discarded(self):
        reader = perf.WireReader(Port([b"\0\x01\0"]))
        self.assertEqual(reader.read(), b"\0")
        self.assertEqual(reader.read(), b"\x01\0")

    def test_aggregation_weights_by_time_and_frame_count(self):
        rows = [dict(stats=dict(window_ms=1000), frames=10,
                     wire_bytes_per_direction=100, instrumented_cycles=6000000,
                     instrumented_cpu_percent=1, baud=1000000),
                dict(stats=dict(window_ms=3000), frames=90,
                     wire_bytes_per_direction=900, instrumented_cycles=36000000,
                     instrumented_cpu_percent=2, baud=1000000)]
        value = audit.aggregate(rows)
        self.assertEqual(value["cpu"], 1.75)  # Not (1 + 2) / 2.
        self.assertEqual(value["cpf"], 420000)
        self.assertEqual(value["wire"], 0.25)


if __name__ == "__main__":
    unittest.main()
