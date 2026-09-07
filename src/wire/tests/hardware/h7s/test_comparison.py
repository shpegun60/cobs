#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
import copy
import json
import unittest
from unittest.mock import patch
import run_comparison as bench
import verify_comparison as verifier


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
        sys.path.insert(0, str(bench.SRC / "wire/tests"))
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


class RecordIdentityTests(unittest.TestCase):
    """Exercise the actual verifier on copies of the committed observations.

    Source provenance has its own test above and CLI validation; these tests
    isolate attribution without needing git history, retained ELFs or a board.
    """

    @classmethod
    def setUpClass(cls):
        cls.records = {
            path.name: json.loads(path.read_text(encoding="utf-8"))
            for path in bench.HERE.glob("results_comparison*.json")
        }

    def setUp(self):
        sources = patch.object(verifier, "verify_sources", return_value=[])
        sources.start()
        self.addCleanup(sources.stop)
        framed = patch.object(bench.rtu, "FRAMED", False)
        framed.start()
        self.addCleanup(framed.stop)

    def record(self, name="results_comparison_framed_2026-09-05.json"):
        return copy.deepcopy(self.records[name])

    def test_saved_records_are_still_accepted(self):
        self.assertGreaterEqual(len(self.records), 6)
        for name in self.records:
            with self.subTest(record=name):
                data = self.record(name)
                core, uart, _ = verifier.verify(data)
                self.assertEqual(len(core), sum(len(run["groups"]) for run in data["core"]))
                self.assertEqual(len(uart), sum(len(run["rows"]) for run in data["uart"]))

    def test_uart_rows_cannot_override_run_identity(self):
        for protocol in bench.PROTOCOLS:
            for field, wrong in (("protocol", "wrong-protocol"), ("policy", "table"), ("baud", 9600)):
                with self.subTest(protocol=protocol, field=field):
                    data = self.record()
                    run = next(r for r in data["uart"] if r["protocol"] == protocol and r["policy"] == "bitwise")
                    run["rows"][0][field] = wrong
                    with self.assertRaisesRegex(AssertionError, f"UART row {field} does not match run"):
                        verifier.verify(data)

    def test_swapped_uart_policy_labels_are_rejected_even_when_the_matrix_is_complete(self):
        for protocol in bench.PROTOCOLS:
            with self.subTest(protocol=protocol):
                data = self.record()
                changed = 0
                for run in data["uart"]:
                    if run["protocol"] == protocol and run["policy"] in ("bitwise", "table"):
                        for row in run["rows"]:
                            row["policy"] = {"bitwise": "table", "table": "bitwise"}[row["policy"]]
                            changed += 1
                self.assertEqual(changed, 4)  # two policies, two repeats; same CRC bytes and matrix keys
                with self.assertRaisesRegex(AssertionError, "UART row policy does not match run"):
                    verifier.verify(data)

    def test_core_groups_cannot_swap_policy_labels(self):
        data = self.record("results_comparison_2026-09-05.json")
        for run in data["core"]:
            if run["policy"] in ("bitwise", "table"):
                for group in run["groups"]:
                    group["policy"] = {"bitwise": "table", "table": "bitwise"}[group["policy"]]
        with self.assertRaisesRegex(AssertionError, "core group policy does not match run"):
            verifier.verify(data)

    def test_uart_run_baud_must_match_hello(self):
        for protocol in bench.PROTOCOLS:
            with self.subTest(protocol=protocol):
                data = self.record()
                run = next(r for r in data["uart"] if r["protocol"] == protocol)
                run["hello"]["baud"] = 9600
                with self.assertRaisesRegex(AssertionError, "UART HELLO baud does not match run"):
                    verifier.verify(data)


if __name__ == "__main__":
    unittest.main()
