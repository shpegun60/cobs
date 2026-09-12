#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
import copy
import hashlib
import struct
import unittest
import verify as v


class OracleTests(unittest.TestCase):
    def test_known_crc(self):
        self.assertEqual(v.crc16(b"123456789"), 0x4B37)
        self.assertEqual(v.crc16(b""), 65535)
        self.assertEqual(v.crc16(bytes.fromhex("01030000000a")), 0xCDC5)

    def test_canonical_full_block(self):
        self.assertEqual(v.encode(b"\x11" * 254), b"\xff" + b"\x11" * 254 + b"\0")
        self.assertEqual(v.encode(b"\x11" * 254 + b"\0"), b"\xff" + b"\x11" * 254 + b"\x01\x01\0")
        self.assertEqual(v.encode(b""), b"\x01\0")

    def test_all_wire_modes(self):
        for p in (0, 1):
            for c in range(4):
                for wide, n in v.SIZES:
                    for pattern in (0, 1):
                        body = v.payload(n, pattern)
                        self.assertEqual(v.unwire(v.wire(body, p, c, wide), p, c, wide), body)
                        if c:
                            self.assertEqual(v.wire(body, p, c, wide), v.wire(body, p, 1, wide))

    def test_crc_rejection(self):
        for p in (0, 1):
            frame = bytearray(v.wire(b"example", p, 1, False))
            frame[-2] ^= 4
            with self.assertRaises(AssertionError):
                v.unwire(frame, p, 1, False)

    def uart_row(self):
        p, c, count = 0, 3, 100
        bodies = v.corpus("mixed")
        total = sum(len(bodies[i % len(bodies)]) for i in range(count))
        def snap(ms, n, size, cycles, irq, calls):
            words = [2, p, c, 0, 600000000, ms, n, size, 0, cycles, 0, irq, 0, calls, 0, 0, 0, 1]
            return dict(words=words, tx=v.wire(v.MAGIC + b"S", p, c, False).hex(),
                        rx=v.wire(v.MAGIC + b"S" + struct.pack("<18I", *words), p, c, False).hex())
        h = hashlib.sha256()
        for i in range(count): h.update(v.wire(bodies[i % len(bodies)], p, c, False))
        return dict(memory=0, case="mixed", repetition=0, requested_seconds=2, host_seconds=2.0,
                    sent=count, received=count, payload_bytes=total, observed_wire_sha256=h.hexdigest(),
                    before=snap(100, 0, 0, 100, 100, 1), after=snap(2100, count, total, 1200100, 120100, 501))

    def test_cpu_formula(self):
        result = v.check_uart(self.uart_row(), 0, 3)
        self.assertAlmostEqual(result["cpu"], 0.11)
        self.assertEqual(result["cycles_per_echo"], 13200)

    def test_corrupted_uart_receipts(self):
        base = self.uart_row()
        for field, value in (("sent", 99), ("received", 0), ("payload_bytes", 1),
                             ("observed_wire_sha256", "0" * 64), ("memory", 1), ("host_seconds", 3.0)):
            row = copy.deepcopy(base)
            row[field] = value
            with self.subTest(field=field), self.assertRaises(AssertionError):
                v.check_uart(row, 0, 3)
        row = copy.deepcopy(base)
        row["after"]["words"][8] = 1
        with self.assertRaises(AssertionError): v.check_uart(row, 0, 3)

    def test_no_vacuous_pass(self):
        with self.assertRaises(AssertionError):
            v.check_lines(dict(protocol=0, policy=3, lines=[]))

    def test_table_weak_readonly(self):
        symbols = "080093f0 00000200 V crc::Engine::lookup_\n"
        linker = " .rodata._engine_lookup_E\n                0x080093f0      0x200 bench.o\n"
        v.check_crc_table(symbols, linker, 2)
        v.check_crc_table(symbols.replace(" V ", " R "), linker, 2)
        v.check_crc_table("", "", 3)
        for table, section, policy in (
                (symbols, linker.replace(".rodata", ".data"), 2),
                (symbols.replace("080093f0", "240093f0"), linker, 2),
                (symbols.replace("00000200", "00000400"), linker, 2),
                (symbols, linker.replace("0x200", "0x400"), 2),
                (symbols + symbols, linker, 2), (symbols, linker, 1), ("", linker, 2)):
            with self.assertRaises(AssertionError): v.check_crc_table(table, section, policy)


if __name__ == "__main__":
    if not __debug__: raise RuntimeError("Python -O is forbidden")
    unittest.main()
