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
                    if size + 2 + trailer + 2 <= 256:
                        framed = bench.wire("rtu-framed", policy, body)
                        self.assertEqual(framed[:4], b"\x11\x41" + size.to_bytes(2, "big"))
                        self.assertEqual(framed[4:4 + size], body)
                        self.assertTrue(bench.policy_for(policy).verify(framed))
                        self.assertEqual(len(framed), size + 4 + trailer)
                    else:
                        with self.assertRaises(AssertionError):
                            bench.wire("rtu-framed", policy, body)

    def test_table_does_not_change_wire(self):
        for protocol in ("cobs", "rtu"):
            for size in bench.SIZES:
                body = bench.payload(size, 0)
                self.assertEqual(bench.wire(protocol, "bitwise", body, 1024),
                                 bench.wire(protocol, "table", body, 1024))

    def test_uart_geometry_selection(self):
        self.assertEqual(bench.parse_cobs_uart("128x8"), ((128, 8),))
        self.assertEqual(bench.parse_cobs_uart("128x8, 256X4"), ((128, 8), (256, 4)))
        with self.assertRaises(AssertionError): bench.parse_cobs_uart("128x8,128x8")
        with self.assertRaises(AssertionError): bench.parse_cobs_uart("32x8")
        with self.assertRaises(AssertionError): bench.parse_cobs_uart("1024x8")
        self.assertEqual(bench.image_tag("cobs", "bitwise", 1000000), "cobs-bitwise-1000000")
        self.assertEqual(bench.image_tag("cobs", "bitwise", 1000000, (256, 4)), "cobs-bitwise-1000000-256x4")
        self.assertEqual(bench.image_tag("rtu", "table", 115200, None), "rtu-table-115200")

    def test_case_sets(self):
        # Old records name no selection and mean the seven original scenarios;
        # random250 exists only for the three-way comparison.
        self.assertEqual(bench.CASES[:len(bench.DEFAULT_CASES)], bench.DEFAULT_CASES)
        self.assertEqual([c[0] for c in bench.CASES if c not in bench.DEFAULT_CASES], ["random250"])
        self.assertEqual(bench.PROTOCOLS, ("cobs", "rtu", "rtu-framed"))
        for policy in bench.POLICIES:
            self.assertEqual(len(bench.wire("rtu-framed", policy, bench.payload(250, 0))),
                             256 - (2 if policy == "none" else 0))

    def test_cadence_budget_for_worst_frame(self):
        for case in bench.CASES:
            for baud in (115200, 1000000):
                fps = bench.target_fps(case, baud)
                self.assertTrue(1 <= fps <= 300)
                for body in bench.corpus(case):
                    for protocol in ("cobs", "rtu"):
                        for policy in bench.POLICIES:
                            self.assertLessEqual(len(bench.wire(protocol, policy, body)) * 20 * fps / baud, 0.75)


    def test_selection_validation(self):
        import verify_comparison as verifier
        good = dict(uart_only=True, protocols=["cobs", "rtu"], policies=["none", "bitwise", "table"],
                    bauds=[1000000], cases=["random252"], cobs_uart=[[128, 8], [256, 4]])
        self.assertEqual(verifier.selection_of(dict(selection=good))["cobs_uart"], ((128, 8), (256, 4)))
        self.assertEqual(verifier.selection_of({})["policies"], bench.POLICIES)  # old full records
        for broken in (dict(policies=[]), dict(protocols=[]), dict(cases=[]), dict(bauds=[]), dict(cobs_uart=[]),
                       dict(cases=["random252", "random252"]), dict(policies=["none", "crc32"]),
                       dict(protocols=["cobs", "tcp"]), dict(bauds=[0]), dict(cobs_uart=[[128, 8], [128, 8]]),
                       dict(cobs_uart=[[32, 8]]), dict(core_only=True)):
            with self.assertRaises(AssertionError, msg=str(broken)):
                verifier.selection_of(dict(selection={**good, **broken}))

    def test_hello_confirms_policy_labels(self):
        import verify_comparison as verifier

        def cobs_run(policy, crc_policy=None):
            hello = {"crc_size": 0 if policy == "none" else 2}
            if crc_policy is not None:
                hello["crc_policy"] = crc_policy
            return {"protocol": "cobs", "policy": policy, "hello": hello}

        def rtu_run(policy, crc_policy):
            return {"protocol": "rtu", "policy": policy, "hello": {"crc_policy": crc_policy}}

        self.assertTrue(verifier.check_hello(rtu_run("bitwise", 0)))
        self.assertTrue(verifier.check_hello(rtu_run("table", 1)))
        self.assertTrue(verifier.check_hello(rtu_run("none", 2)))
        with self.assertRaises(AssertionError): verifier.check_hello(rtu_run("table", 0))
        # harness protocol 3: the board names its policy
        self.assertTrue(verifier.check_hello(cobs_run("bitwise", 1)))
        self.assertTrue(verifier.check_hello(cobs_run("table", 2)))
        with self.assertRaises(AssertionError): verifier.check_hello(cobs_run("table", 1))
        with self.assertRaises(AssertionError): verifier.check_hello(cobs_run("none", 1))
        # harness protocol 2 records: only the trailer width is available
        self.assertFalse(verifier.check_hello(cobs_run("bitwise")))
        self.assertFalse(verifier.check_hello(cobs_run("table")))
        with self.assertRaises(AssertionError): verifier.check_hello({"protocol": "cobs", "policy": "bitwise", "hello": {"crc_size": 0}})

    def test_provenance_never_accepts_an_older_version(self):
        import contextlib, hashlib, io, subprocess, sys, tempfile
        from pathlib import Path
        sys.path.insert(0, str(bench.REPO / "wire/tests"))
        from provenance import Provenance

        def digest(text):
            return hashlib.sha256(text.encode()).hexdigest()

        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
            repo = Path(tmp)

            def git(*args):
                return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True).stdout.strip()

            git("init", "-q")
            git("config", "user.email", "test@example.invalid"); git("config", "user.name", "test")
            git("config", "commit.gpgsign", "false")
            commits = []
            for version in ("v1", "v2", "v3"):
                (repo / "src.h").write_text(version)
                git("add", "src.h"); git("commit", "-q", "-m", version)
                commits.append(git("rev-parse", "HEAD"))
            older, base, later = commits
            provenance = Provenance(repo)
            self.assertEqual(provenance.committed_match(base, "src.h", digest("v2")), base)
            self.assertEqual(provenance.committed_match(base, "src.h", digest("v3")), later)
            self.assertIsNone(provenance.committed_match(base, "src.h", digest("v1")), "older than the base")
            self.assertIsNone(provenance.committed_match(base, "src.h", digest("v4")))
            with self.assertRaises(AssertionError):
                provenance.check(base, {"src.h": digest("v1")})
            provenance.check(base, {"src.h": digest("v3")})
            self.assertEqual(provenance.later, {(base[:12], later[:12]): {"src.h"}})
            (repo / "src.h").write_text("v4")  # measured but not committed: accepted only with a CAVEAT
            provenance.check(base, {"src.h": digest("v4")})
            self.assertEqual(provenance.uncommitted, {"src.h"})
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(any(line.startswith("CAVEAT") for line in provenance.report()))


if __name__ == "__main__":
    unittest.main()
