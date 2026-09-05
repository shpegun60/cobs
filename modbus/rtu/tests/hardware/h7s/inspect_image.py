#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""Bind live results to the exact flashed load image and its CRC disassembly."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

REPO = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO / "crc/tests"))
from codegen_tools import (  # noqa: E402
    DEFAULT_CXX, call_instructions, function_body, instructions,
    lookup_tables, run, sha256, sibling,
)
from modbus_hardware import CRC_POLICIES  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--cxx", default=DEFAULT_CXX)
    parser.add_argument("--policy", choices=tuple(CRC_POLICIES), required=True)
    parser.add_argument("--baud", type=int, required=True)
    parser.add_argument("--optimization", choices=("Os", "O2", "O3"), default="Os")
    parser.add_argument("--lto", type=int, choices=(0, 1), default=0)
    args = parser.parse_args()
    policy = CRC_POLICIES[args.policy]
    elf = args.elf.resolve()
    binary = elf.with_suffix(".bin")
    run(sibling(args.cxx, "objcopy"), "-O", "binary", elf, binary)
    disassembly = run(sibling(args.cxx, "objdump"), "-d", elf)
    entry = "modbus_crc_benchmark_probe"
    pending = [entry]
    bodies = {}
    direct_crc_calls = []
    while pending:
        symbol = pending.pop()
        if symbol in bodies:
            continue
        body = function_body(disassembly, symbol)
        if not instructions(body):
            raise AssertionError(f"empty CRC function: {symbol}")
        bodies[symbol] = body
        for call in call_instructions(body):
            target = re.search(r"<([^>]+)>", call)
            if not target:
                raise AssertionError(f"indirect CRC call: {call}")
            callee = target[1]
            if callee == symbol:
                continue  # Back edge to the start of this function.
            if not re.match(r"_ZNK?3crc", callee):
                raise AssertionError(f"unexpected external CRC helper: {call}")
            direct_crc_calls.append({"from": symbol, "to": callee, "instruction": call})
            pending.append(callee)
    tables = lookup_tables(
        args.cxx, elf, 256 * policy.wire_size
        if args.policy.endswith("table") else 0,
    )
    sizes = run(sibling(args.cxx, "size"), elf).splitlines()[1].split()
    manifest = {
        "policy": args.policy,
        "policy_id": policy.identifier,
        "baud": args.baud,
        "optimization": "-" + args.optimization,
        "lto": bool(args.lto),
        "compiler": run(args.cxx, "--version").splitlines()[0],
        "source_base_commit": run("git", "-C", REPO, "rev-parse", "HEAD").strip(),
        "source_sha256": {
            str(path.relative_to(REPO)).replace("\\", "/"): sha256(path)
            for path in (REPO / "crc/Crc.h", REPO / "modbus/rtu/Rtu.h",
                         Path(__file__).with_name("modbus_bench.cpp"),
                         Path(__file__).with_name("build.sh"),
                         *(REPO / "modbus/rtu").glob("*.h"),
                         *(REPO / "modbus/rtu/detail").glob("*.h"),
                         *(REPO / "wire").glob("*.h"),
                         *(REPO / "wire/detail").glob("*.h"))
        },
        "binary_sha256": sha256(binary),
        "binary_bytes": binary.stat().st_size,
        "text_bytes": int(sizes[0]),
        "data_bytes": int(sizes[1]),
        "bss_bytes": int(sizes[2]),
        "tables": tables,
        "lookup_bytes": sum(table["bytes"] for table in tables),
        "probe_static_instructions": sum(len(instructions(b)) for b in bodies.values()),
        "probe_entry_static_instructions": len(instructions(bodies[entry])),
        "probe_disassembly": bodies,
        "probe_direct_crc_calls": direct_crc_calls,
        "probe_helper_calls": [],
    }
    destination = elf.with_suffix(".image.json")
    destination.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"IMAGE {args.policy}: {manifest['binary_sha256']}; "
        f"{manifest['probe_static_instructions']} static CRC instructions, "
        f"{manifest['lookup_bytes']} lookup B, "
        f"{len(direct_crc_calls)} direct outlined CRC calls, no external helper"
    )


if __name__ == "__main__":
    main()
