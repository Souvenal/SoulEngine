#!/usr/bin/env python3
"""CLI tests for the ADR checker."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHECKER = REPO_ROOT / "Tools" / "check_adr.py"


def run_checker(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), str(path)], capture_output=True, text=True, cwd=REPO_ROOT
    )


class AdrCheckerTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def write_fixture(self, name: str, text: str) -> Path:
        path = Path(self._tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_natural_prose_passes(self) -> None:
        path = self.write_fixture("02-example.md", "# ADR 02 — Example" + chr(10) * 2 + "One natural Markdown paragraph." + chr(10))
        result = run_checker(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hard_wrapped_prose_fails(self) -> None:
        text = "# ADR 02 — Example" + chr(10) * 2 + "First half of one paragraph" + chr(10) + "continues on a second line." + chr(10)
        path = self.write_fixture("02-example.md", text)
        result = run_checker(path)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"{path}:4", result.stdout)
        self.assertIn("hard-wrapped ADR prose", result.stdout)

    def test_title_number_must_match_filename(self) -> None:
        path = self.write_fixture("02-example.md", "# ADR 03 — Example" + chr(10) * 2 + "Paragraph." + chr(10))
        result = run_checker(path)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("must match filename number", result.stdout)

    def test_structural_lines_are_exempt(self) -> None:
        text = "# ADR 02 — Example" + chr(10) * 2 + "- First list item" + chr(10) + "- Second list item" + chr(10) * 2 + "~~~text" + chr(10) + "first code line" + chr(10) + "second code line" + chr(10) + "~~~" + chr(10)
        path = self.write_fixture("02-example.md", text)
        result = run_checker(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
