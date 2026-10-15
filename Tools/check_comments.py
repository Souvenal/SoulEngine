#!/usr/bin/env python3
"""Mechanical comment-style check for docs/standards/comments.md.

Errors (exit 1):
  - pure comment lines longer than 110 characters

Warnings (reported, never fail):
  - multi-line /// blocks without @brief (the banned intermediate form)

Code lines with trailing comments are not width-checked (the limit applies to
comment lines, not code). Run from the repo root: python3 Tools/check_comments.py
"""
import glob
import os
import sys

WIDTH = 110
PATTERNS = (
    "Engine/Source/**/*.h",
    "Engine/Source/**/*.hpp",
    "Engine/Source/**/*.cpp",
    "Engine/Source/**/*.cppm",
    "Engine/Shaders/**/*.slang",
    "Applications/*/Shaders/**/*.slang",
)


def iter_files():
    seen = set()
    for pattern in PATTERNS:
        for path in glob.glob(pattern, recursive=True):
            if path not in seen:
                seen.add(path)
                yield path


def main() -> int:
    errors = []
    warnings = []
    for path in iter_files():
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().split("\n")
        for index, line in enumerate(lines, 1):
            stripped = line.lstrip()
            if len(line) > WIDTH and stripped.startswith("//"):
                errors.append(f"{path}:{index}: comment line is {len(line)} chars (>{WIDTH})")
        index = 0
        while index < len(lines):
            stripped = lines[index].lstrip()
            if stripped.startswith("///") and not stripped.startswith("///<"):
                end = index
                while end < len(lines) and lines[end].lstrip().startswith("///"):
                    end += 1
                block = lines[index:end]
                if len(block) >= 2 and not any("@brief" in entry for entry in block):
                    warnings.append(f"{path}:{index + 1}: multi-line /// block without @brief")
                index = end
            else:
                index += 1
    for entry in warnings:
        print(f"warning: {entry}")
    for entry in errors:
        print(f"error: {entry}")
    print(f"check_comments: {len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
