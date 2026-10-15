#!/usr/bin/env python3
"""Fixture-based CLI seam tests for Tools/check_comments.py.

Run from the repo root: uv run python Tools/test_check_comments.py

Tests assert external behavior only: exit codes, file:line diagnostics on
stdout, and post---fix file contents. They never import the checker.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHECKER = REPO_ROOT / "Tools" / "check_comments.py"


def run_checker(*args, cwd=None):
    return subprocess.run(
        [sys.executable, str(CHECKER), *args],
        capture_output=True,
        text=True,
        cwd=cwd or REPO_ROOT,
    )


class CheckerCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write_fixture(self, name, content):
        path = self.dir / name
        path.write_text(content, encoding="utf-8")
        return path


class WidthRuleTest(CheckerCase):
    def test_pure_comment_line_over_110_is_error(self):
        fixture = self.write_fixture("wide.cpp", "// " + "x" * 120 + "\n")
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"{fixture}:1", result.stdout)
        self.assertIn(">110", result.stdout)

    def test_comment_line_at_exactly_110_is_ok(self):
        fixture = self.write_fixture("ok.cpp", "// " + "x" * 107 + "\n")
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_code_line_with_trailing_comment_is_exempt(self):
        fixture = self.write_fixture(
            "code.cpp", "int value = 0; // " + "x" * 120 + "\n"
        )
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)


class ArgvFileListTest(CheckerCase):
    def test_only_listed_files_are_checked(self):
        clean = self.write_fixture("clean.cpp", "// short comment\n")
        dirty = self.write_fixture("dirty.cpp", "// " + "x" * 120 + "\n")
        result = run_checker(str(clean))
        self.assertEqual(result.returncode, 0, result.stdout)
        result = run_checker(str(dirty))
        self.assertEqual(result.returncode, 1, result.stdout)
        result = run_checker(str(clean), str(dirty))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"{dirty}:1", result.stdout)


class EarlyWrapRuleTest(CheckerCase):
    def test_joinable_double_slash_pair_is_error(self):
        fixture = self.write_fixture(
            "wrap.cpp",
            "// This comment was wrapped early.\n"
            "// It would fit on one line.\n",
        )
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"{fixture}:1", result.stdout)
        self.assertIn("wrap", result.stdout)

    def test_joinable_triple_slash_pair_is_error(self):
        fixture = self.write_fixture(
            "wrap3.cpp",
            "/// This doc comment was wrapped early.\n"
            "/// It would fit on one line.\n",
        )
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"{fixture}:1", result.stdout)

    def test_pair_joined_over_110_is_ok(self):
        first = "// " + "a" * 60
        second = "// " + "b" * 60
        fixture = self.write_fixture("long.cpp", first + "\n" + second + "\n")
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_indented_pair_uses_own_indent_for_join_width(self):
        # Joined length includes the leading indent; 8-space indent plus two
        # 60-char bodies exceeds 110, so no violation.
        first = "        // " + "a" * 55
        second = "        // " + "b" * 55
        fixture = self.write_fixture("ind.cpp", first + "\n" + second + "\n")
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)


class EarlyWrapExclusionTest(CheckerCase):
    def assert_clean(self, content):
        fixture = self.write_fixture("excl.cpp", content)
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_list_items_are_excluded(self):
        self.assert_clean("// - first item of a list\n// - second item of a list\n")
        self.assert_clean(
            "/// @brief Has bullets.\n"
            "///\n"
            "/// * first bullet point\n"
            "/// * second bullet point\n"
        )

    def test_at_command_lines_are_excluded(self):
        # @command as the second line of the pair.
        self.assert_clean("// plain prose line here\n// @note follows the prose\n")
        # @command as the first line of the pair.
        self.assert_clean("// @note precedes the prose\n// plain prose line here\n")

    def test_trailing_note_lines_are_excluded(self):
        self.assert_clean("///< trailing note one\n///< trailing note two\n")

    def test_blank_triple_slash_separator_is_excluded(self):
        self.assert_clean(
            "/// @brief Has details.\n"
            "///\n"
            "/// First detail paragraph.\n"
            "///\n"
            "/// Second detail paragraph.\n"
        )

    def test_aligned_continuation_is_excluded(self):
        self.assert_clean(
            "/// @brief Has a parameter.\n"
            "///\n"
            "/// @param name a description that runs long and\n"
            "///               continues aligned under the text\n"
        )
        # Aligned continuation without an @command on the first line.
        self.assert_clean(
            "// a leading line of prose\n"
            "//     continuation indented for alignment\n"
        )

    def test_mixed_markers_do_not_join(self):
        self.assert_clean("// plain comment line\n/// doc comment line\n")


class BannerRuleTest(CheckerCase):
    def test_box_drawing_decoration_line_is_error(self):
        fixture = self.write_fixture(
            "banner.cpp",
            "// " + "\u2550" * 40 + "\n"
            "//  Banner Title\n"
            "// " + "\u2550" * 40 + "\n"
            "int value;\n",
        )
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"{fixture}:1", result.stdout)
        self.assertIn(f"{fixture}:3", result.stdout)
        self.assertIn("banner", result.stdout.lower())

    def test_two_box_chars_are_not_a_banner(self):
        fixture = self.write_fixture("ok.cpp", "// a\u2500\u2500b \u2500\u2500c\n")
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_various_box_drawing_glyphs_flagged(self):
        for glyph in ("\u2500", "\u2501", "\u2502", "\u2550", "\u2551"):
            fixture = self.write_fixture("g.cpp", "// " + glyph * 5 + "\n")
            result = run_checker(str(fixture))
            self.assertEqual(result.returncode, 1, glyph)


class BriefRuleTest(CheckerCase):
    def test_multi_line_doc_block_without_brief_is_error(self):
        fixture = self.write_fixture(
            "brief.cpp",
            "/// First line of prose documentation.\n"
            "/// Second line of prose documentation.\n"
            "void Function();\n",
        )
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"{fixture}:1", result.stdout)
        self.assertIn("@brief", result.stdout)

    def test_multi_line_doc_block_with_brief_is_ok(self):
        fixture = self.write_fixture(
            "brief.cpp",
            "/// @brief Does a thing.\n"
            "///\n"
            "/// Detail paragraph explaining the thing in one reasonably sized line.\n"
            "void Function();\n",
        )
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_single_line_doc_comment_is_ok(self):
        fixture = self.write_fixture("brief.cpp", "/// @brief Does a thing.\n")
        result = run_checker(str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)


class FixReflowTest(CheckerCase):
    def test_fix_joins_early_wrapped_pair(self):
        fixture = self.write_fixture(
            "join.cpp",
            "// This comment was wrapped early.\n"
            "// It should join into one line.\n",
        )
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(
            fixture.read_text(encoding="utf-8"),
            "// This comment was wrapped early. It should join into one line.\n",
        )

    def test_fix_rewraps_paragraph_to_fill_110(self):
        words = [f"w{n:07d}" for n in range(1, 21)]  # 8 chars each
        lines = ["// " + " ".join(words[i : i + 5]) for i in range(0, 20, 5)]
        fixture = self.write_fixture("para.cpp", "\n".join(lines) + "\n")
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        expected = (
            "// " + " ".join(words[:12]) + "\n"
            "// " + " ".join(words[12:]) + "\n"
        )
        self.assertEqual(fixture.read_text(encoding="utf-8"), expected)
        # The first output line fills the ceiling exactly: 3 + 12*8 + 11.
        self.assertEqual(len("// " + " ".join(words[:12])), 110)

    def test_fix_keeps_indent_and_marker(self):
        fixture = self.write_fixture(
            "ind.cpp",
            "    /// This doc line was wrapped early and\n"
            "    /// continues on a second line here.\n",
        )
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(
            fixture.read_text(encoding="utf-8"),
            "    /// This doc line was wrapped early and continues on a second line here.\n",
        )

    def test_fix_does_not_join_excluded_lines(self):
        original = (
            "// intro words stay alone\n"
            "// - item one of a list\n"
            "// - item two of a list\n"
            "// trailing prose stays alone\n"
        )
        fixture = self.write_fixture("excl.cpp", original)
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(fixture.read_text(encoding="utf-8"), original)

    def test_fix_does_not_touch_code(self):
        original = "int value = 0; // trailing comment\n"
        fixture = self.write_fixture("code.cpp", original)
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(fixture.read_text(encoding="utf-8"), original)


class FixBannerTest(CheckerCase):
    def test_fix_degrades_banner_and_preserves_title(self):
        fixture = self.write_fixture(
            "banner.cpp",
            "// " + "\u2550" * 40 + "\n"
            "//  Banner Title\n"
            "// " + "\u2550" * 40 + "\n"
            "int value;\n",
        )
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(
            fixture.read_text(encoding="utf-8"),
            "// Banner Title\nint value;\n",
        )

    def test_fix_deletes_lone_decoration_line(self):
        fixture = self.write_fixture(
            "lone.cpp",
            "// " + "\u2500" * 40 + "\n"
            "// normal comment stays\n",
        )
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(fixture.read_text(encoding="utf-8"), "// normal comment stays\n")

    def test_fix_leaves_unfixable_errors_failing(self):
        fixture = self.write_fixture(
            "brief.cpp",
            "/// First line of prose documentation.\n"
            "/// Second line of prose documentation that is quite a bit longer and cannot join.\n"
            "void Function();\n",
        )
        result = run_checker("--fix", str(fixture))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("@brief", result.stdout)


class FixIdempotenceTest(CheckerCase):
    def test_fix_is_idempotent(self):
        fixture = self.write_fixture(
            "mix.cpp",
            "// " + "\u2550" * 40 + "\n"
            "//  Section Header\n"
            "// " + "\u2550" * 40 + "\n"
            "int first;\n"
            "// This paragraph was wrapped far too early by an\n"
            "// author stuck in the eighty column habit.\n"
            "int second;\n",
        )
        first = run_checker("--fix", str(fixture))
        self.assertEqual(first.returncode, 0, first.stdout)
        after_first = fixture.read_text(encoding="utf-8")
        second = run_checker("--fix", str(fixture))
        self.assertEqual(second.returncode, 0, second.stdout)
        self.assertEqual(fixture.read_text(encoding="utf-8"), after_first)
        self.assertEqual(
            after_first,
            "// Section Header\n"
            "int first;\n"
            "// This paragraph was wrapped far too early by an author stuck in the eighty column habit.\n"
            "int second;\n",
        )


class GlobFallbackTest(CheckerCase):
    def test_no_args_globs_the_repository(self):
        result = run_checker(cwd=REPO_ROOT)
        self.assertIn(result.returncode, (0, 1), result.stdout)
        self.assertIn("check_comments:", result.stdout)


if __name__ == "__main__":
    unittest.main()
