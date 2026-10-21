#!/usr/bin/env python3
"""Generate fail-fast definitions for SDK calls not scripted by the native fixture.

Prototypes come from the locally installed SDK and remain build artifacts. A
missing prototype fails generation; this tool never links a real SDK fallback.
"""

import argparse
from pathlib import Path
import re
import subprocess


def symbols(nm, option, objects):
    """Read plain C SDK symbols from compiled production/fixture objects."""
    output = subprocess.check_output([nm, option, *objects], text=True)
    return set(re.findall(r"\b[A-Za-z] (skiq_\w+)\b", output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk-headers", nargs="+", type=Path, required=True)
    parser.add_argument("--objects", nargs="+", required=True)
    args = parser.parse_args()
    required = symbols(args.nm, "--undefined-only", args.objects)
    defined = symbols(args.nm, "--defined-only", args.objects)
    needed = required - defined
    prototypes = {}
    for directory in args.sdk_headers:
        for header in sorted(directory.glob("*.h")):
            source = re.sub(r"/\*.*?\*/|//[^\n]*", "", header.read_text(), flags=re.S)
            for match in re.finditer(
                r"EPIQ_API\s+([\w\s*]+?)\b(skiq_\w+)\s*\((.*?)\)\s*;", source, re.S
            ):
                prototypes[match[2]] = (match[1].strip(), match[3].strip())
    missing = needed - prototypes.keys()
    if missing:
        raise RuntimeError("SDK prototypes missing for: " + ", ".join(sorted(missing)))
    lines = [
        "// Generated from local SDK declarations; never commit this file.",
        "#include <sidekiq_api.h>",
        "#include <cstdio>",
        "#include <cstdlib>",
        '#pragma GCC diagnostic ignored "-Wunused-parameter"',
        'extern "C" {',
    ]
    for name in sorted(needed):
        result, parameters = prototypes[name]
        lines.extend([
            f"{result} {name}({parameters}) {{",
            f'    std::fprintf(stderr, "Unexpected scripted SDK call: {name}\\n");',
            "    std::abort();",
            "}",
        ])
    lines.append("}")
    args.output.write_text("\n".join(lines) + "\n")
    print(f"Generated {len(needed)} fail-fast SDK definitions; no SDK library fallback")


if __name__ == "__main__":
    main()
