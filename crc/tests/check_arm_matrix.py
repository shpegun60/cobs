#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""Compile/disassemble all nine CRC policies across GNU Arm CPU targets.

This is AArch32 code-generation evidence, not execution on unconnected CPUs.
Each object includes all 16 wire-codec probes and the complete NoCrc verifier.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import re
import subprocess
import time

from codegen_tools import (
    DEFAULT_CXX, call_instructions, function_body, instructions,
    lookup_tables, run, sha256, sibling,
)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
POLICIES = {"nocrc": ("CRC_NO_CRC_PROBE", 0)}
for bits in (8, 16, 32, 64):
    POLICIES[f"crc{bits}-bitwise"] = (f"CRC_BITWISE_{bits}_PROBE", 0)
    POLICIES[f"crc{bits}-table"] = (f"CRC_TABLE_{bits}_PROBE", 256 * bits // 8)
CODECS = [
    f"crc_{operation}_{order}{bits}"
    for bits in (8, 16, 32, 64) for order in ("le", "be")
    for operation in ("store", "load")
]


def instruction_set(cpu: str) -> str:
    # STAR-MC1 is also an M-profile/Thumb-only target; not every M-profile
    # CPU in GCC's list has a "cortex-m" marketing name.
    return "thumb" if cpu.startswith("cortex-m") or cpu == "star-mc1" else "arm"


def supported_cpus(cxx: str) -> list[str]:
    # Query the installed compiler rather than maintaining a stale CPU list.
    result = subprocess.run(
        [cxx, "-mcpu=__crc_audit_list__", "-fsyntax-only", "-x", "c++", "-"],
        input="", text=True, capture_output=True, check=False,
    )
    match = re.search(r"valid arguments are: ([^\n]+)", result.stderr)
    if not match:
        raise RuntimeError(f"cannot enumerate CPU targets:\n{result.stderr}")
    return match[1].strip().split()


def has_runtime_branch(body: str) -> bool:
    for ins in instructions(body):
        mnemonic = ins.split()[0].split(".")[0]
        if mnemonic in ("cbz", "cbnz", "tbb", "tbh") or re.fullmatch(
            r"b(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)", mnemonic
        ):
            return True
    return False


def check_case(cxx: str, out: Path, cpu: str, opt: str, endian: str,
               policy: str, strict: bool = False) -> dict:
    stem = f"{cpu}_{opt}_{endian}_{policy}" + ("_strict" if strict else "")
    obj = out / (stem + ".o")
    assembly = out / (stem + ".asm")
    define, table_bytes = POLICIES[policy]
    flags = [
        "-c", "-std=gnu++20", "-DNDEBUG", "-Wall", "-Wextra", "-Wpedantic",
        "-Wshadow", "-Wconversion", "-Wsign-conversion", "-Werror",
        "-fno-exceptions", "-fno-rtti", "-ffunction-sections", "-fdata-sections",
        f"-{opt}", f"-mcpu={cpu}", "-m" + instruction_set(cpu),
        "-mfloat-abi=soft", f"-m{endian}-endian", "-I" + str(REPO),
        "-D" + define, str(HERE / "crc_codegen.cpp"), "-o", str(obj),
    ]
    if strict:
        flags.insert(0, "-mno-unaligned-access")
    run(cxx, *flags)
    disassembly = run(sibling(cxx, "objdump"), "-dr", obj)
    assembly.write_text(disassembly, encoding="utf-8")
    tables = lookup_tables(cxx, obj, table_bytes)
    body = function_body(disassembly, "crc_policy_calculate_probe")
    decoded = instructions(body)
    if not decoded:
        raise AssertionError(f"{stem}: empty CRC calculation probe")
    calls = call_instructions(body)
    if calls:
        raise AssertionError(f"{stem}: algorithm helper calls: {calls}")
    if policy == "nocrc" and any(re.match(r"^(?:ldr|str|ldm|stm|push|pop)", i) for i in decoded):
        raise AssertionError(f"{stem}: NoCrc calculation accesses memory")
    codec_counts = {}
    for codec in CODECS:
        codec_body = function_body(disassembly, codec)
        if not instructions(codec_body) or call_instructions(codec_body) or \
                has_runtime_branch(codec_body):
            raise AssertionError(f"{stem}: non-inline/non-constant codec {codec}:\n{codec_body}")
        codec_counts[codec] = len(instructions(codec_body))
    no_crc_body = function_body(disassembly, "crc_no_crc_verify_probe")
    no_crc_ops = instructions(no_crc_body)
    if not no_crc_ops or call_instructions(no_crc_body) or has_runtime_branch(no_crc_body) or \
            any(re.match(r"^(?:ldr|str|ldm|stm|push|pop)", i) for i in no_crc_ops):
        raise AssertionError(f"{stem}: NoCrc verifier did not fold away:\n{no_crc_body}")
    if not any(re.match(r"mov(?:s|\.w)? r0,\s*#1$", ins) for ins in no_crc_ops):
        raise AssertionError(f"{stem}: NoCrc verifier is not constant true:\n{no_crc_body}")
    return {
        "cpu": cpu, "isa": instruction_set(cpu),
        "optimization": "-" + opt, "endian": endian, "policy": policy,
        "strict_alignment": strict, "status": "passed",
        "object_sha256": sha256(obj), "disassembly_sha256": sha256(assembly),
        "calculate_static_instructions": len(decoded),
        "lookup_bytes": sum(table["bytes"] for table in tables),
        "codec_static_instructions": codec_counts,
        "no_crc_verify_static_instructions": len(no_crc_ops),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=DEFAULT_CXX)
    parser.add_argument("--cpus", nargs="+", help="default: every named CPU accepted by this compiler")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--output", type=Path, default=HERE / "out/all-arm")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    supported = supported_cpus(args.cxx)
    cpus = args.cpus or supported
    if set(cpus) - set(supported):
        parser.error(f"unsupported CPUs: {set(cpus) - set(supported)}")
    args.output.mkdir(parents=True, exist_ok=True)
    tasks = [
        (cpu, opt, endian, policy, False)
        for cpu in cpus for opt in ("Os", "O2", "O3")
        for endian in ("little", "big") for policy in POLICIES
    ]
    # Explicit strict-unaligned mode for every CPU, width and wire order.
    # All 16 codec variants are present even in the NoCrc policy object.
    tasks += [
        (cpu, opt, endian, "nocrc", True)
        for cpu in cpus for opt in ("Os", "O2", "O3") for endian in ("little", "big")
    ]
    report = args.report or args.output / "results.json"
    if report.exists():
        parser.error(f"refusing to overwrite an existing audit: {report}")
    started = time.monotonic()
    records = []
    failures = []
    print(f"Auditing {len(cpus)} CPU targets / {len(tasks)} objects", flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_cases = {
            executor.submit(check_case, args.cxx, args.output, *case): case
            for case in tasks
        }
        for future in as_completed(future_cases):
            case = future_cases[future]
            try:
                records.append(future.result())
            except Exception as error:
                failure = {"case": case, "status": "failed", "error": str(error)}
                failures.append(failure)
                print(f"FAIL {case}: {error}", flush=True)
            completed = len(records) + len(failures)
            if completed % 100 == 0 or completed == len(tasks):
                print(
                    f"{completed}/{len(tasks)}: {len(records)} passed, "
                    f"{len(failures)} failed, {time.monotonic() - started:.1f}s",
                    flush=True,
                )
    result = {
        "compiler": run(args.cxx, "--version").splitlines()[0],
        "source_base_commit": run("git", "-C", REPO, "rev-parse", "HEAD").strip(),
        "source_sha256": {
            str(path.relative_to(REPO)).replace("\\", "/"): sha256(path)
            for path in (REPO / "crc/Crc.h", HERE / "crc_codegen.cpp",
                         Path(__file__), HERE / "codegen_tools.py")
        },
        "scope": "GNU AArch32 CPU targets; M uses Thumb, other targets ARM; soft-float; no LTO",
        "cpus": cpus,
        "seconds": time.monotonic() - started,
        "passed": len(records), "failed": len(failures),
        "cases": sorted(records, key=lambda r: (
            r["cpu"], r["optimization"], r["endian"], r["policy"], r["strict_alignment"]
        )),
        "failures": failures,
    }
    report.write_text(json.dumps(result, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"Report: {report}", flush=True)
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
