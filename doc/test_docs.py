#!/usr/bin/env python3
"""Author: shpegun60. SPDX-License-Identifier: MIT. Negative controls for the docs gate."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import check_docs as docs


class NavigationTests(unittest.TestCase):
    def test_headings_ignore_code(self):
        self.assertEqual(docs.headings("# Title\n```cpp\n## Not a heading\n```\n## Real\n"),
                         [(1, "Title", "title"), (2, "Real", "real")])

    def test_duplicate_and_unicode_anchors(self):
        self.assertEqual(docs.anchors("## Політики без магії\n## Repeat\n## Repeat\n"),
                         {"політики-без-магії", "repeat", "repeat-1"})

    def test_code_format_and_punctuation(self):
        self.assertEqual(docs.slug("`read_be()` / `read_le()`"), "read_be--read_le")

    def test_contents_is_idempotent(self):
        original = "# Title\n\nIntro.\n\n## One\n\nText\n### Two\n"
        formatted = docs.format_toc(original)
        self.assertEqual(formatted, docs.format_toc(formatted))
        self.assertIn("[One](#one)", formatted)
        self.assertIn("  - [Two](#two)", formatted)

    def test_links_missing_files_and_anchors(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            path = root / "guide.md"
            (root / "target.md").write_text("# Target\n## Real heading\n", encoding="utf-8")
            errors = []
            with patch.object(docs, "ROOT", root):
                count = docs.links(path, "[ok](target.md#real-heading)\n[bad](missing.md)\n[bad](target.md#wrong)", errors)
            self.assertEqual(count, 3)
            self.assertEqual(len(errors), 2)
            self.assertTrue(any("missing target" in e for e in errors))
            self.assertTrue(any("missing heading/anchor" in e for e in errors))

    def test_external_and_code_are_not_local_links(self):
        errors = []
        count = docs.links(docs.ROOT / "README.md", "[web](https://example.com/x)\n```cpp\n[x](missing.md)\n```", errors)
        self.assertEqual((count, errors), (0, []))

    def test_escape_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "escapes repository"):
            docs.inside_root(docs.ROOT / ".." / "outside.md")

    def test_preserved_route_removal_fails_even_if_the_target_exists(self):
        path = docs.ROOT / "doc/TESTING.md"
        targets = docs.PRESERVED_LINKS["doc/TESTING.md"]
        complete = "\n".join(f"[record]({target})" for target in targets)
        errors = []
        docs.preserved_links(path, complete, errors)
        self.assertEqual(errors, [])
        removed = "../src/cobs/tests/bench/README.md"
        docs.preserved_links(path, complete.replace(f"[record]({removed})", ""), errors)
        self.assertEqual(len(errors), 1)
        self.assertIn(removed, errors[0])
        self.assertTrue((path.parent / removed).exists())

    def test_fenced_link_does_not_restore_navigation(self):
        errors = []
        docs.preserved_links(docs.ROOT / "README.md", "```md\n[guide](doc/USER_GUIDE.md)\n```", errors)
        self.assertTrue(any("doc/USER_GUIDE.md" in error for error in errors))

    def test_readme_keeps_project_identity(self):
        text = (docs.ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("# CRC, COBS, Modbus RTU/TCP + STM32 DMA UART for C++20", text)
        self.assertIn("[![C++20]", text)
        self.assertIn("[![STM32]", text)
        self.assertIn("[![License: MIT]", text)
        self.assertIn("Author: shpegun60", text)

    def test_unmatched_generated_blocks(self):
        with self.assertRaisesRegex(ValueError, "unmatched example"):
            docs.format_excerpts(docs.ROOT / "guide.md", "<!-- example: x.cpp#part -->")
        with self.assertRaisesRegex(ValueError, "contents marker"):
            docs.format_toc("# Title\n<!-- toc -->\n")

    def test_excerpt_refresh_and_missing_region(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            source = root / "sample.cpp"
            source.write_text("// example-begin: part\n    int value = 1;\n// example-end: part\n", encoding="utf-8")
            old = "<!-- example: sample.cpp#part -->\nOLD\n<!-- /example -->"
            with patch.object(docs, "ROOT", root):
                new = docs.format_excerpts(root / "guide.md", old)
                self.assertIn("```cpp\nint value = 1;\n```", new)
                self.assertNotIn("OLD", new)
                self.assertEqual(new, docs.format_excerpts(root / "guide.md", new))
                with self.assertRaisesRegex(ValueError, "missing/duplicate"):
                    docs.format_excerpts(root / "guide.md", old.replace("#part", "#absent"))


if __name__ == "__main__":
    unittest.main()
