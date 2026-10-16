#!/usr/bin/env python3
"""Mechanical comment-style gate for docs/standards/comments.md.

Errors (exit 1):
  - pure comment lines longer than 110 characters
  - early-wrapped comments: two adjacent pure comment lines that would join
    into one line of <=110 characters (exclusions: list items, @command
    lines, ///< trailing notes, blank /// separators, and aligned
    continuations whose content carries leading indentation)
  - banner lines: comment lines with >=3 consecutive box-drawing characters
  - multi-line /// blocks without @brief (the banned intermediate form)

Code lines with trailing comments are not width-checked (the limit applies to
comment lines, not code). --fix reflows early-wrapped paragraphs and degrades
banner blocks. Run from the repo root: python3 Tools/check_comments.py
"""
import argparse
import glob
import re
import sys

WIDTH = 110
BOX_DRAWING = re.compile(r"[\u2500-\u257F]{3,}")
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


def classify(line):
    """Split a pure comment line into (indent, marker, raw) or return None."""
    stripped = line.lstrip()
    if not stripped.startswith("//"):
        return None
    indent = line[: len(line) - len(stripped)]
    if stripped.startswith("///<"):
        return indent, "///<", stripped[4:]
    if stripped.startswith("///"):
        return indent, "///", stripped[3:]
    return indent, "//", stripped[2:]


def reflow_target(line):
    """Return (indent, marker, content) for lines the early-wrap rule joins.

    Returns None for non-comment lines and every exclusion class: ///<
    trailing notes, blank /// separators, aligned continuations (more than
    one whitespace character after the marker), list items, @command lines,
    and banner lines (handled by their own rule).
    """
    parsed = classify(line)
    if parsed is None:
        return None
    indent, marker, raw = parsed
    if marker == "///<":
        return None
    if raw.strip() == "":
        return None
    if raw[:1].isspace():
        if raw[0] == "\t" or (len(raw) > 1 and raw[1].isspace()):
            return None
        content = raw[1:]
    else:
        content = raw
    if content in ("-", "*") or content.startswith("- ") or content.startswith("* "):
        return None
    if content.startswith("@"):
        return None
    if BOX_DRAWING.search(line):
        return None
    return indent, marker, content


def is_banner_line(line):
    return classify(line) is not None and BOX_DRAWING.search(line) is not None


def degrade_banners(lines):
    """Delete decoration lines; demote a sandwiched title to plain // text."""
    out = []
    index = 0
    while index < len(lines):
        if not is_banner_line(lines[index]):
            out.append(lines[index])
            index += 1
            continue
        # Look for a closing decoration line within the same comment run.
        cursor = index + 1
        while cursor < len(lines) and classify(lines[cursor]) is not None:
            if is_banner_line(lines[cursor]):
                break
            cursor += 1
        closed = cursor < len(lines) and classify(lines[cursor]) is not None
        if closed:
            for title in lines[index + 1 : cursor]:
                parsed = classify(title)
                text = parsed[2].strip()
                if text:
                    out.append(f"{parsed[0]}// {text}")
            index = cursor + 1
        else:
            index += 1
    return out


def reflow_paragraphs(lines):
    """Greedily re-wrap runs of eligible comment lines to fill WIDTH."""
    out = []
    index = 0
    while index < len(lines):
        target = reflow_target(lines[index])
        if target is None:
            out.append(lines[index])
            index += 1
            continue
        indent, marker, _ = target
        words = []
        end = index
        while end < len(lines):
            target = reflow_target(lines[end])
            if target is None or target[0] != indent or target[1] != marker:
                break
            words.extend(target[2].split())
            end += 1
        prefix = f"{indent}{marker} "
        current = ""
        for word in words:
            if not current:
                current = word
            elif len(prefix) + len(current) + 1 + len(word) <= WIDTH:
                current += " " + word
            else:
                out.append(prefix + current)
                current = word
        if current:
            out.append(prefix + current)
        index = end
    return out


def fix_lines(lines):
    return reflow_paragraphs(degrade_banners(lines))


def check_lines(path, lines):
    errors = []
    for index, line in enumerate(lines, 1):
        if classify(line) is None:
            continue
        if len(line) > WIDTH:
            errors.append(f"{path}:{index}: comment line is {len(line)} chars (>{WIDTH})")
        if is_banner_line(line):
            errors.append(f"{path}:{index}: box-drawing banner line (>=3 consecutive box-drawing chars)")
    for index in range(len(lines) - 1):
        first = reflow_target(lines[index])
        second = reflow_target(lines[index + 1])
        if first is None or second is None:
            continue
        if first[0] != second[0] or first[1] != second[1]:
            continue
        joined = f"{first[0]}{first[1]} {first[2]} {second[2]}"
        if len(joined) <= WIDTH:
            errors.append(
                f"{path}:{index + 1}: comment line wraps early; joins with next line within {WIDTH} chars"
            )
    index = 0
    while index < len(lines):
        stripped = lines[index].lstrip()
        if stripped.startswith("///") and not stripped.startswith("///<"):
            end = index
            while end < len(lines) and lines[end].lstrip().startswith("///"):
                end += 1
            block = lines[index:end]
            if len(block) >= 2 and not any("@brief" in entry for entry in block):
                errors.append(f"{path}:{index + 1}: multi-line /// block without @brief")
            index = end
        else:
            index += 1
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fix",
        action="store_true",
        help="reflow early-wrapped paragraphs and degrade banner blocks in place",
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="files to check; defaults to globbing the repository",
    )
    args = parser.parse_args()
    paths = args.files if args.files else list(iter_files())
    errors = []
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().split("\n")
        if args.fix:
            fixed = fix_lines(lines)
            if fixed != lines:
                with open(path, "w", encoding="utf-8") as handle:
                    handle.write("\n".join(fixed))
                lines = fixed
        errors.extend(check_lines(path, lines))
    for entry in errors:
        print(f"error: {entry}")
    print(f"check_comments: {len(errors)} error(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
