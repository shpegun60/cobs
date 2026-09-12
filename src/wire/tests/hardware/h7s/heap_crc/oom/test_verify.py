#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
import copy
import unittest
import verify


class ObservationTests(unittest.TestCase):
    def row(self, key="N"):
        hello = "HELLO,1,600000000,603979776,131072,0,0"
        stages = ["EXHAUSTED", "BEFORE"] + (["ABORT", "EXIT"] if key in verify.FATAL else
                  ["MALLOC_NULL" if key == "M" else "POOL_OK", "RECOVERED"])
        lines = []
        for stage in stages:
            count, size = (0, 0) if stage == "RECOVERED" else (60, 129000)
            detail = 1 if stage == "EXIT" or (stage == "BEFORE" and key in ("G", "g")) else 0
            lines.append(f"{stage},{key},{count},{size},4,131072,0,{detail}")
        return dict(command=key, hello=hello, lines=lines, after_ping="" if key in verify.FATAL else hello)

    def test_all_expected_observations(self):
        for key in verify.PLAN: verify.check_case(self.row(key))

    def test_silence_is_not_abort_proof(self):
        for length in (0, 1, 2, 3):
            row = self.row(); row["lines"] = row["lines"][:length]
            with self.assertRaises(AssertionError): verify.check_case(row)

    def test_missing_exhaustion_and_false_recovery(self):
        for index, before, after in ((0, ",129000,", ",1000,"), (0, ",4,", ",0,"),
                                     (3, ",0,1", ",0,0"), (2, "ABORT", "RETURNED_NULL")):
            row = self.row(); row["lines"][index] = row["lines"][index].replace(before, after)
            with self.assertRaises(AssertionError): verify.check_case(row)
        row = self.row(); row["after_ping"] = row["hello"]
        with self.assertRaises(AssertionError): verify.check_case(row)

    def test_no_control_hang_accepted(self):
        row = self.row("P"); row["after_ping"] = ""
        with self.assertRaises(AssertionError): verify.check_case(row)

    def test_real_disassembly_and_missing_forward(self):
        from pathlib import Path
        path = verify.REPO / "stm32_cube_test/h7s_cobs_test/out/heap-oom/bench.dis"
        if not path.exists(): self.skipTest("build OOM image to check local disassembly")
        text = path.read_text()
        verify.check_disassembly(text)
        for old, new in (("<abort>", "<fake_abort>"), ("<__wrap__exit>", "<fake_exit>")):
            with self.assertRaises(AssertionError): verify.check_disassembly(text.replace(old, new))


if __name__ == "__main__":
    if not __debug__: raise RuntimeError("Python -O is forbidden")
    unittest.main()
