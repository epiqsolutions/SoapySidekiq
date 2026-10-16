#!/usr/bin/env python3
"""Check changed C++ lines against clang-format 14 without rewriting files."""

import argparse
import difflib
import os
from pathlib import Path
import re
import subprocess
import sys


def output(*command):
    """Run a command in the repository and propagate failures to the caller."""
    return subprocess.check_output(command, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/master", help="Git base revision")
    parser.add_argument("--clang-format", default="clang-format-14")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    # Subprocesses and git paths must refer to the same checkout, from any cwd.
    os.chdir(root)
    version = output(args.clang_format, "--version")
    if not re.search(r"\bversion 14\.", version):
        raise RuntimeError("Use clang-format 14 for reproducible formatting")
    # Validate the configuration even when this change contains no C++ hunks.
    output(args.clang_format, "--style=file", "--dump-config")
    base = output("git", "merge-base", args.base, "HEAD").strip()
    names = output(
        "git", "diff", "--no-ext-diff", "--name-only", "-z", "--diff-filter=ACMRT",
        "--no-renames", base, "--", "*.cpp", "*.hpp",
    )
    failed = False
    checked = 0
    for name in filter(None, names.split("\0")):
        patch = output(
            "git", "diff", "--no-ext-diff", "--no-color", "--unified=0",
            "--no-renames", base, "--", name,
        )
        ranges = []
        for start, count in re.findall(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", patch, re.M):
            length = int(count) if count else 1
            if length:
                ranges.append(f"--lines={start}:{int(start) + length - 1}")
        if not ranges:
            continue
        checked += 1
        before = Path(name).read_text()
        after = output(args.clang_format, "--style=file", *ranges, str(root / name))
        if before != after:
            failed = True
            sys.stdout.writelines(difflib.unified_diff(
                before.splitlines(keepends=True), after.splitlines(keepends=True),
                fromfile=name, tofile=f"{name} (formatted)",
            ))
    print(f"Formatting: checked changed lines in {checked} C++ files")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Formatting check failed: {error}", file=sys.stderr)
        sys.exit(2)
