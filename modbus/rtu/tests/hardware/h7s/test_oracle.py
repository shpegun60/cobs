#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""Offline checks of the PC oracle, benchmark protocol and object reader."""

import random
import struct
import sys
from pathlib import Path
import unittest
import zlib

from modbus_hardware import (
    CRC_POLICIES, HardwareLink, make_adu, parse_adu, reference_self_check,
)

sys.path.insert(0, str(Path(__file__).resolve().parents[5] / "crc/tests"))
from codegen_tools import function_body, instructions, call_instructions  # noqa: E402


class OracleTests(unittest.TestCase):
    def test_all_nine_models(self):
        for name, policy in list(CRC_POLICIES.items())[:9]:
            with self.subTest(policy=name):
                reference_self_check(policy)
                maximum = make_adu(policy, 1, 0x43, bytes(policy.max_data_size))
                self.assertEqual(len(maximum), 256)
                with self.assertRaises(ValueError):
                    make_adu(policy, 1, 0x43, bytes(policy.max_data_size + 1))
                for invalid_size in (0, 1, 257):
                    with self.assertRaises(AssertionError):
                        parse_adu(policy, bytes(invalid_size))

    def test_external_crc32_oracle(self):
        rng = random.Random(0xC32)
        for size in range(257):
            data = rng.randbytes(size)
            self.assertEqual(CRC_POLICIES["crc32-bitwise"].calculate(data), zlib.crc32(data))

    def test_modbus_canonical_wire(self):
        self.assertEqual(
            make_adu(CRC_POLICIES["bitwise"], 1, 3, bytes.fromhex("00 00 00 0A")),
            bytes.fromhex("01 03 00 00 00 0A C5 CD"),
        )

    def test_nocrc_no_trailer_no_detection(self):
        policy = CRC_POLICIES["nocrc"]
        self.assertEqual(make_adu(policy, 1, 3), b"\x01\x03")
        self.assertEqual(parse_adu(policy, b"\x00\x03").address, 0)
        self.assertEqual(parse_adu(policy, b"\x01\x03altered").data, b"altered")

    def test_benchmark_response(self):
        class MockLink(HardwareLink):
            def control(self, command, payload_size, arguments=(), timeout=5):
                size, iterations = arguments
                checksum = self.policy.calculate(bytes(
                    (index * 37 + 0xA5) & 255 for index in range(size)
                ))
                mix = sum(checksum ^ index for index in range(iterations)) \
                    & 0xFFFFFFFFFFFFFFFF
                return struct.pack(
                    "<IIIQQQIII", 0, size, iterations, 1000, mix,
                    checksum, 1, 3 << 16, 0x411FC271,
                )

        for name, policy in list(CRC_POLICIES.items())[:9]:
            link = MockLink(None, policy)
            for size in (0, 1, 246, 256):
                with self.subTest(policy=name, size=size):
                    self.assertEqual(link.crc_benchmark(size)["cycles"], 1000)
            for size, iterations in ((257, 8), (-1, 8), (1, 9), (1, 0)):
                with self.assertRaises(ValueError):
                    link.crc_benchmark(size, iterations)

    def test_disassembly_parser(self):
        text = """
00000000 <probe>:
   0:	2000      	movs	r0, #0
   2:	4410      	add	r0, r2
   4:	f7ff fffe 	bl	0 <helper>
   8:	4770      	bx	lr
   a:	bf00      	nop
   c:	12345678 	.word	0x12345678
			c: R_ARM_ABS32 lookup

00000010 <helper>:
  10:	4770      	bx	lr
"""
        body = function_body(text, "probe")
        self.assertEqual(len(instructions(body)), 5)
        self.assertEqual(instructions(body)[1], "add r0, r2")
        self.assertEqual(call_instructions(body), ["bl 0 <helper>"])
        with self.assertRaises(AssertionError):
            function_body(text, "missing")


if __name__ == "__main__":
    unittest.main()
