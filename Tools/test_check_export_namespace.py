#!/usr/bin/env python3
"""CLI tests for the export namespace checker."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHECKER = REPO_ROOT / "Tools" / "check_export_namespace.py"


def run_checker(*args):
    return subprocess.run(
        [sys.executable, str(CHECKER), *args],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
    )


class ExportNamespaceCheckerTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.fixture = Path(self._tmp.name) / "fixture.cppm"

    def test_explicit_exports_inside_ordinary_namespace_pass(self):
        self.fixture.write_text(
            "namespace SoulEngine {\nexport class ShaderType {};\n}\n",
            encoding="utf-8",
        )

        result = run_checker(str(self.fixture))

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")

    def test_export_namespace_fails_with_location(self):
        self.fixture.write_text(
            "export module Fixture;\n  export namespace SoulEngine {}\n",
            encoding="utf-8",
        )

        result = run_checker(str(self.fixture))

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"{self.fixture}:2", result.stdout)
        self.assertIn("export namespace", result.stdout)


if __name__ == "__main__":
    unittest.main()
