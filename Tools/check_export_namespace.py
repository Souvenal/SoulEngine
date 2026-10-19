#!/usr/bin/env python3
"""Reject export namespace declarations in C++ module source files."""

import re
import sys
from pathlib import Path

PATTERN = re.compile(r"\bexport\s+namespace\b")


def main() -> int:
    errors = []
    for filename in sys.argv[1:]:
        path = Path(filename)
        with path.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if PATTERN.search(line):
                    errors.append(
                        f"{path}:{line_number}: forbidden export namespace declaration"
                    )
    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
