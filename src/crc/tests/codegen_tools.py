#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""Small, shared GNU object/disassembly reader for reproducible CRC audits."""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import subprocess


DEFAULT_CXX = (
    "C:/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/"
    "com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32."
    "14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
)


def run(*args: str | Path) -> str:
    result = subprocess.run(
        [str(arg) for arg in args], text=True, capture_output=True, check=False
    )
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, args))}\n{result.stdout}{result.stderr}")
    return result.stdout


def sibling(cxx: str, name: str) -> str:
    return cxx.replace("g++", name)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function_body(disassembly: str, name: str) -> str:
    match = re.search(
        rf"^[0-9a-fA-F]+ <{re.escape(name)}>:\n(.*?)(?=\n\n|\Z)",
        disassembly, flags=re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise AssertionError(f"missing disassembly symbol: {name}")
    return match[1].rstrip()


def instructions(body: str) -> list[str]:
    # GNU objdump: address TAB hex halfwords/words TAB mnemonic TAB operands.
    # Literal pools (.word etc.) and relocation lines are not instructions.
    result = []
    for line in body.splitlines():
        match = re.match(
            r"^\s*[0-9a-fA-F]+:\s+(?:(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{4}|[0-9a-fA-F]{2})\s+)+"
            r"([a-z][a-z0-9.]*)\s*(.*)$", line
        )
        if match:
            result.append(f"{match[1]} {match[2]}".rstrip())
    return result


def call_instructions(body: str) -> list[str]:
    return [
        ins for ins in instructions(body)
        if re.match(r"^(?:bl|blx)(?:\.[a-z]+)?\s", ins)
        or re.match(r"^b(?:\.[a-z]+)?\s.*<", ins)
        and not re.search(r"<[^>]+\+0x[0-9a-f]+>", ins)
    ]


def lookup_tables(cxx: str, path: Path, expected_bytes: int) -> list[dict]:
    symbols = run(sibling(cxx, "nm"), "-S", "--defined-only", "-C", path)
    tables = []
    for line in symbols.splitlines():
        if "crc::detail::Engine<" not in line or "::lookup_" not in line:
            continue
        match = re.match(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\w)\s+(.*)$", line)
        if not match:
            raise AssertionError(f"unparsed CRC lookup symbol: {line}")
        address, size, kind, name = match.groups()
        if kind.lower() not in ("r", "v"):
            raise AssertionError(f"CRC table is not read-only: {line}")
        tables.append({"address": int(address, 16), "bytes": int(size, 16), "name": name})
    if len(tables) != (1 if expected_bytes else 0) or \
            sum(table["bytes"] for table in tables) != expected_bytes:
        raise AssertionError(f"unexpected lookup emission: {tables}; expected {expected_bytes} B")
    if tables:
        table_sections = [
            line for line in run(sibling(cxx, "objdump"), "-t", "-C", path).splitlines()
            if "crc::detail::Engine<" in line and "::lookup_" in line
        ]
        # STM32's linker collects .rodata into FLASH .text.
        if len(table_sections) != 1 or not re.search(
            r"\sO\s+\.(?:rodata\S*|text)\s", table_sections[0]
        ):
            raise AssertionError(f"CRC lookup has an unexpected section: {table_sections}")
    return tables
