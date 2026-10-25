#!/usr/bin/env python3
"""Validate ADR naming, titles, and prose paragraph formatting."""

import re
import sys
from pathlib import Path

FILENAME_PATTERN = re.compile(r"^(?P<number>[0-9]{2})-[a-z0-9][a-z0-9-]*\.md$")
TITLE_PATTERN = re.compile(r"^# ADR (?P<number>[0-9]{2}) — .+$")
STRUCTURAL_PATTERN = re.compile(r"^(#|[-*+] |[0-9]+\. |>|\|| {2,}|\x60\x60\x60|~~~)")


def is_prose(line: str, in_fence: bool) -> bool:
    return bool(line) and not in_fence and not STRUCTURAL_PATTERN.match(line)


def check(path: Path) -> list[str]:
    errors: list[str] = []
    filename_match = FILENAME_PATTERN.match(path.name)
    if not filename_match:
        return [f"{path}: ADR filename must use NN-slug.md"]

    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        return [f"{path}: ADR must not be empty"]

    title_match = TITLE_PATTERN.match(lines[0])
    if not title_match:
        errors.append(f"{path}:1: ADR title must use '# ADR NN — Title'")
    elif title_match.group("number") != filename_match.group("number"):
        errors.append(f"{path}:1: ADR title number must match filename number")

    in_fence = False
    previous_was_prose = False
    for line_number, line in enumerate(lines, 1):
        if line.startswith(("```", "~~~")):
            in_fence = not in_fence
            previous_was_prose = False
            continue
        prose = is_prose(line, in_fence)
        if prose and previous_was_prose:
            errors.append(
                f"{path}:{line_number}: hard-wrapped ADR prose; keep each logical paragraph on one line"
            )
        previous_was_prose = prose
    return errors


def main() -> int:
    errors = []
    for filename in sys.argv[1:]:
        errors.extend(check(Path(filename)))
    print("\n".join(errors))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
