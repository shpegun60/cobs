#!/usr/bin/env python3
"""Author: shpegun60. SPDX-License-Identifier: MIT.

Check maintained Markdown links/anchors and format generated contents/excerpts.
--write only rewrites generated Markdown blocks; never source or saved evidence.
No network, third-party Python dependencies or build/flash side effects.
"""
from __future__ import annotations

import argparse
import re
import textwrap
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
MANAGED = (
    "README.md", "doc/README.md", "doc/START_HERE_UK.md", "doc/USER_GUIDE.md",
    "doc/INTEGRATION.md", "doc/EXAMPLES.md", "doc/QT.md", "doc/FREERTOS.md",
    "doc/TESTING.md", "doc/BUILD.md", "doc/API_PARITY.md", "doc/STORAGE.md",
    "doc/ARCHITECTURE.md", "doc/PROTOCOL.md", "doc/PAYLOAD_LIMITS.md",
    "src/modbus/README.md", "src/modbus/ARCHITECTURE.md", "src/modbus/tcp/README.md",
    "src/crc/README.md", "doc/LEGACY_REVIEW.md", "doc/examples/README.md",
    "src/adapters/README.md", "src/cobs/README.md", "src/wire/README.md", "src/uart/README.md",
    "doc/DOC_PRESERVATION.md",
)
TOC = re.compile(r"<!-- toc -->.*?<!-- /toc -->", re.S)
EXCERPT = re.compile(r"<!-- example: ([^\n]+) -->.*?<!-- /example -->", re.S)
# These routes were once silently removed while every remaining link still passed.
# An intentional move needs a replacement route here and in DOC_PRESERVATION.md.
PRESERVED_LINKS = {
    "README.md": {"doc/USER_GUIDE.md", "doc/DOC_PRESERVATION.md"},
    "doc/README.md": {"DOC_PRESERVATION.md", "TESTING.md"},
    "doc/INTEGRATION.md": {"examples/rtu_uart_direct.cpp", "examples/rtu_adapter.cpp"},
    "doc/TESTING.md": {
        "../src/cobs/tests/bench/README.md",
        "../src/cobs/tests/qmake_consumer/main.cpp",
        "../src/modbus/rtu/tests/qmake_consumer/main.cpp",
        "../src/cobs/tests/hardware/h7s/results_audited_2026-09-01.jsonl",
        "../src/cobs/tests/hardware/h7s/results_format_api_2026-09-01.jsonl",
        "../src/uart/tests/bench/results_default128x8_10M_audited_2026-09-01.csv",
    },
}


def without_fences(text: str) -> str:
    lines = []
    fence = ""
    for line in text.splitlines():
        match = re.match(r"\s*(`{3,}|~{3,})", line)
        if match:
            marker = match[1][0]
            if not fence:
                fence = marker
            elif marker == fence:
                fence = ""
            lines.append("")
        else:
            lines.append("" if fence else line)
    return "\n".join(lines)


def slug(title: str) -> str:
    title = re.sub(r"<[^>]+>", "", title)
    title = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", title)
    title = title.lower().replace("&amp;", "&")
    return "".join(c for c in title if c.isalnum() or c in " _-").replace(" ", "-")


def headings(text: str) -> list[tuple[int, str, str]]:
    result = []
    seen: dict[str, int] = {}
    for match in re.finditer(r"^(#{1,6})\s+(.+?)\s*#*\s*$", without_fences(text), re.M):
        title = match[2]
        base = slug(title)
        suffix = seen.get(base, 0)
        seen[base] = suffix + 1
        result.append((len(match[1]), title, base + (f"-{suffix}" if suffix else "")))
    return result


def anchors(text: str) -> set[str]:
    found = {anchor for _, _, anchor in headings(text)}
    found.update(re.findall(r'(?:id|name)=["\']([^"\']+)["\']', without_fences(text)))
    return found


def inside_root(path: Path) -> Path:
    path = path.resolve()
    if not path.is_relative_to(ROOT):
        raise ValueError(f"target escapes repository: {path}")
    return path


def format_excerpts(path: Path, text: str) -> str:
    if (text.count("<!-- example:") != len(EXCERPT.findall(text)) or
            text.count("<!-- /example -->") != len(EXCERPT.findall(text))):
        raise ValueError(f"{path.name}: unmatched example marker")
    def replace(match: re.Match[str]) -> str:
        source_name, sep, region = match[1].partition("#")
        if not sep:
            raise ValueError(f"{path.name}: excerpt needs source#region")
        source = inside_root(path.parent / source_name).read_text(encoding="utf-8")
        start = f"// example-begin: {region}"
        end = f"// example-end: {region}"
        if source.count(start) != 1 or source.count(end) != 1:
            raise ValueError(f"{source_name}: missing/duplicate region {region}")
        body = source.split(start, 1)[1].split(end, 1)[0].strip("\n")
        body = textwrap.dedent(body).rstrip()
        return f"<!-- example: {match[1]} -->\n```cpp\n{body}\n```\n<!-- /example -->"
    return EXCERPT.sub(replace, text)


def format_toc(text: str) -> str:
    if text.count("<!-- toc -->") != text.count("<!-- /toc -->") or text.count("<!-- toc -->") > 1:
        raise ValueError("unmatched/duplicate contents marker")
    # Headings in code examples are not Markdown navigation destinations.
    entries = headings(text)
    rows = []
    for level, title, anchor in entries:
        if level not in (2, 3):
            continue
        label = title.replace("`", "")
        rows.append(f"{'  ' if level == 3 else ''}- [{label}](#{anchor})")
    block = "<!-- toc -->\n\nContents\n\n" + "\n".join(rows) + "\n\n<!-- /toc -->"
    if TOC.search(text):
        return TOC.sub(lambda _: block, text, count=1)
    first = re.search(r"^# .+$", text, re.M)
    if not first:
        raise ValueError("document has no title")
    return text[:first.end()] + "\n\n" + block + text[first.end():]


def link_targets(text: str) -> list[str]:
    plain = without_fences(text)
    # Inline links and reference definitions. Titles may follow a quoted space;
    # targets containing spaces must use the normal Markdown <...> form.
    targets = re.findall(r"!?\[[^\]\n]*\]\((<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\)", plain)
    targets += re.findall(r"^\s*\[[^]]+\]:\s*(<[^>]+>|\S+)", plain, re.M)
    return [target.removeprefix("<").removesuffix(">") for target in targets]


def preserved_links(path: Path, text: str, errors: list[str]) -> None:
    name = path.relative_to(ROOT).as_posix()
    for target in sorted(PRESERVED_LINKS.get(name, set()) - set(link_targets(text))):
        errors.append(f"{name}: preserved navigation removed: {target}")


def links(path: Path, text: str, errors: list[str]) -> int:
    count = 0
    for target in link_targets(text):
        url = urlsplit(target)
        if url.scheme or url.netloc:
            continue # external URLs are references, not checked by this offline gate
        count += 1
        try:
            destination = inside_root(path.parent / unquote(url.path)) if url.path else path
            if not destination.exists():
                raise ValueError("missing target")
            if url.fragment and destination.suffix.lower() == ".md":
                if unquote(url.fragment) not in anchors(destination.read_text(encoding="utf-8")):
                    raise ValueError("missing heading/anchor")
        except (OSError, ValueError) as error:
            errors.append(f"{path.relative_to(ROOT).as_posix()}: {target}: {error}")
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="format generated contents/excerpts, then check")
    args = parser.parse_args()
    errors: list[str] = []
    documents = []
    for name in MANAGED:
        path = ROOT / name
        try:
            old = path.read_text(encoding="utf-8")
            new = format_toc(format_excerpts(path, old))
            if old != new:
                if args.write:
                    path.write_text(new, encoding="utf-8", newline="\n")
                    print(f"formatted {name}")
                else:
                    errors.append(f"{name}: generated block stale; run python -B doc/check_docs.py --write")
            documents.append((path, new if args.write else old))
        except (OSError, ValueError) as error:
            errors.append(f"{name}: {error}")
    total = sum(links(path, text, errors) for path, text in documents)
    for path, text in documents:
        preserved_links(path, text, errors)
    for error in errors:
        print(f"FAIL {error}")
    if errors:
        print(f"Documentation: {len(errors)} error(s)")
        return 1
    print(f"Documentation: {len(documents)} maintained documents, {total} local links/anchors, all generated excerpts and contents PASS")
    print("Scope: offline maintained guides; source targets and historical report anchors exist; external URLs/history-internal links are not certified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
