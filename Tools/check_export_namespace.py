#!/usr/bin/env python3
"""Namespace rules for C++ source files.

- `export namespace` declarations are forbidden;
- each file contains at most one `namespace SoulEngine { ... }` block.
"""

import re
import sys
from pathlib import Path

EXPORT_PATTERN = re.compile(r"\bexport\s+namespace\b")
BLOCK_PATTERN = re.compile(r"^namespace SoulEngine \{")


def main() -> int:
    errors = []
    for filename in sys.argv[1:]:
        path = Path(filename)
        blocks = 0
        with path.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if EXPORT_PATTERN.search(line):
                    errors.append(
                        f"{path}:{line_number}: forbidden export namespace declaration"
                    )
                if BLOCK_PATTERN.match(line):
                    blocks += 1
                    if blocks > 1:
                        errors.append(
                            f"{path}:{line_number}: duplicate namespace SoulEngine block; merge it into the first one"
                        )
    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
