#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Receipt verifier negative controls, without a board or source mutation."""
import contextlib
import copy
import io
import json
import tempfile
from pathlib import Path
import unittest
import verify


class ReceiptTests(unittest.TestCase):
    def test_complete_and_corrupted_receipts(self):
        self.check_receipt(verify.HERE / "results_2026-09-12/session.json")
        self.check_receipt(verify.HERE / "results_payload_limits_2026-09-12/session.json")

    def check_receipt(self, receipt):
        original = json.loads(receipt.read_text())
        mutations = [
            lambda r: r.update(completed=False),
            lambda r: r.update(restored_and_verified=False),
            lambda r: r.update(readback_sha256="0" * 64),
            lambda r: r["images"].pop(),
            lambda r: r["images"][0].update(config="1-0"),
            lambda r: r["images"][0]["positive"].pop(),
            lambda r: r["images"][0]["positive"][0].update(received=""),
            lambda r: r["images"][0]["positive"][0].update(chunks=[]),
            lambda r: r["images"][0]["negative"].pop(),
            lambda r: r["images"][0]["negative"][0].update(received="00"),
            lambda r: r["images"][0]["negative"][0].update(baseline=""),
            lambda r: r["images"][0]["negative"][0].update(boot_after=""),
            lambda r: r["images"][0].update(self_test="SELF,4094,1,2,0"),
            lambda r: r["images"][1].update(self_test="SELF,4094,0,14,1"),
            lambda r: r["images"][0].update(binary_bytes=65537),
            lambda r: r["images"][0]["artifacts"].pop("elf"),
        ]
        if original["schema"] == 2:
            mutations += [
                lambda r: r.update(limit_kind="ADU bytes"),
                lambda r: r.update(max_data_size=1023),
                lambda r: r["images"][0].update(hello="TCP,2,600000000,0,0,1024,196608,1024"),
                lambda r: r["images"][0].update(self_test="SELF,4094,0,2,0,53,1"),
            ]
        with tempfile.TemporaryDirectory(prefix="tcp-receipt-") as folder:
            path = Path(folder) / "record.json"
            path.write_text(json.dumps(original))
            with contextlib.redirect_stdout(io.StringIO()):
                verify.verify(path)
            for index, mutate in enumerate(mutations):
                with self.subTest(mutation=index):
                    record = copy.deepcopy(original)
                    mutate(record)
                    path.write_text(json.dumps(record))
                    with self.assertRaises(AssertionError), contextlib.redirect_stdout(io.StringIO()):
                        verify.verify(path)
        print(f"TCP schema {original['schema']}: valid control + {len(mutations)} intentional corruptions checked")


if __name__ == "__main__":
    if not __debug__: raise SystemExit("Python -O is forbidden")
    unittest.main()
