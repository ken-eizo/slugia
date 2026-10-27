#!/usr/bin/env python3

import argparse
import hashlib
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--namespace", default="slugia::assets")
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()

    data = args.input.read_bytes()
    values = [f"0x{value:02x}" for value in data]
    lines = ["#pragma once", "", "#include <cstdint>", ""]
    for part in args.namespace.split("::"):
        lines.append(f"namespace {part} {{")
    lines.append(f"inline constexpr std::uint8_t {args.symbol}[] = {{")
    for index in range(0, len(values), 20):
        lines.append("  " + ", ".join(values[index:index + 20]) + ",")
    lines.append("};")
    lines.append(f'inline constexpr char {args.symbol}Sha256[] = "{hashlib.sha256(data).hexdigest()}";')
    for part in reversed(args.namespace.split("::")):
        lines.append(f"}} // namespace {part}")
    lines.append("")
    generated = "\n".join(lines)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() and args.output.read_text(encoding="utf-8") == generated:
        # MSBuild decides whether a custom command is stale from timestamps. A content-stable
        # output older than the input/script otherwise reruns this ~1 MB font generator forever.
        newest_input = max(args.input.stat().st_mtime, Path(__file__).stat().st_mtime)
        if args.output.stat().st_mtime < newest_input:
            args.output.touch()
        return 0
    with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", newline="\n", delete=False,
            dir=args.output.parent, prefix=args.output.name + ".", suffix=".tmp") as stream:
        stream.write(generated)
        temporary = Path(stream.name)
    temporary.replace(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
