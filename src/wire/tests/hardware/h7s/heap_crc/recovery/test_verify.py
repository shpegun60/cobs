#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
import unittest
import verify as v


class RecoveryTests(unittest.TestCase):
    def row(self, key):
        hello = "HELLO,2,600000000,603983776,131072,0,0"
        lines = []
        def stage(tag, detail=0):
            count, payload = (0, 0) if tag == "RECOVERED" else (52, 130642)
            lines.append(f"{tag},{key},{count},{payload},4,131072,0,{detail}")
        def wire(protocol, size): lines.append(f"WIRE,{key},{protocol},{size},{v.wire(protocol, size).hex()}")
        if key in v.RX: wire(v.RX[key], 2)
        stage("EXHAUSTED"); stage("BEFORE", (4 if key == "J" else 2) if key in v.GROW else 0)
        if key == "N": stage("ABORT"); stage("EXIT", 1)
        else:
            if key == "M": stage("MALLOC_NULL")
            elif key == "P": stage("POOL_OK")
            else: stage("REFUSED", 5 if key in v.GROW else 3 if key in v.RX else 6 if key == "Z" else 1)
            if key in v.GROW: wire(v.GROW[key], 34)
            for p in (0, 1, 2): wire(p, 2)
            stage("RECOVERED")
        return dict(command=key, hello=hello, lines=lines, after_ping="" if key == "N" else hello)

    def test_exact_plan(self):
        for key in v.PLAN: v.check_case(self.row(key))

    def test_known_oracles(self):
        self.assertEqual(v.crc16(b"123456789"), bytes.fromhex("374b"))
        self.assertEqual(v.wire(0, 2), bytes.fromhex("06044142b1d100"))

    def test_no_silence_or_missing_recovery(self):
        for key in v.PLAN:
            r = self.row(key); r["lines"].pop()
            with self.assertRaises(AssertionError): v.check_case(r)
        r = self.row("C"); r["after_ping"] = ""
        with self.assertRaises(AssertionError): v.check_case(r)

    def test_bad_data_and_failed_checks(self):
        r = self.row("G")
        index = next(i for i, x in enumerate(r["lines"]) if x.startswith("WIRE,"))
        r["lines"][index] = r["lines"][index][:-4] + "ffff"
        with self.assertRaises(AssertionError): v.check_case(r)
        r = self.row("c"); r["lines"][3] = r["lines"][3].replace(",0,3", ",1,3")
        with self.assertRaises(AssertionError): v.check_case(r)

    def test_no_abort_in_fixed_paths(self):
        for key in ("C", "R", "F", "c", "r", "f", "G", "g", "J", "Z"):
            r = self.row(key); index = next(i for i, x in enumerate(r["lines"]) if x.startswith("REFUSED,"))
            r["lines"][index] = r["lines"][index].replace("REFUSED,", "ABORT,")
            with self.assertRaises(AssertionError): v.check_case(r)

    def test_elf_paths(self):
        path = v.REPO / "stm32_cube_test/h7s_cobs_test/out/heap-recovery/bench.dis"
        if not path.exists(): self.skipTest("build the recovery image first")
        text = path.read_text(); v.check_disassembly(text)
        with self.assertRaises(AssertionError): v.check_disassembly(text.replace("<free>", "<operator delete(void*)>"))


if __name__ == "__main__":
    if not __debug__: raise RuntimeError("Python -O is forbidden")
    unittest.main()
