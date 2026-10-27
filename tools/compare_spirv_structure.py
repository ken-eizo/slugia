"""Compare stripped disassembly in instruction order up to numeric-ID renaming.

This is a syntactic comparison after SDK specialization/optimization, not a
proof about the driver's machine code or general shader equivalence.
"""
import argparse
from collections import Counter
import difflib
import hashlib
import json
from pathlib import Path
import re


TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|[^\s]+')
ID = re.compile(r'%[0-9]+\Z')


def normalize(path):
    mapping = {}
    lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith(";"):
            continue
        tokens = []
        for token in TOKEN.findall(line):
            if ID.fullmatch(token):
                token = mapping.setdefault(token, "%" + str(len(mapping) + 1))
            tokens.append(token)
        if tokens:
            lines.append(" ".join(tokens))
    text = "\n".join(lines)
    if "OpMemoryModel" not in text or "OpEntryPoint" not in text:
        raise ValueError("Expected a shader disassembly, not an empty comparison")
    return text, len(mapping)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("legacy", type=Path)
    parser.add_argument("trial", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    before, before_ids = normalize(args.legacy)
    after, after_ids = normalize(args.trial)
    def opcodes(text):
        counts = Counter()
        for line in text.splitlines():
            tokens = line.split()
            opcode = tokens[2] if tokens[0].startswith("%") else tokens[0]
            if not opcode.startswith("Op"):
                raise ValueError("Malformed disassembly instruction")
            counts[opcode] += 1
        return counts
    before_ops, after_ops = opcodes(before), opcodes(after)
    result = {"scope": __doc__.strip(), "identical_up_to_id_renaming": before == after,
              "legacy": str(args.legacy), "trial": str(args.trial),
              "legacy_ids": before_ids, "trial_ids": after_ids,
              "opcode_histograms_equal": before_ops == after_ops,
              "legacy_opcode_histogram": dict(sorted(before_ops.items())),
              "trial_opcode_histogram": dict(sorted(after_ops.items())),
              "legacy_normalized_sha256": hashlib.sha256(before.encode()).hexdigest(),
              "trial_normalized_sha256": hashlib.sha256(after.encode()).hexdigest()}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))
    if before != after:
        print("\n".join(list(difflib.unified_diff(before.splitlines(), after.splitlines()))[:32]))
    return 0 if before == after else 1


if __name__ == "__main__":
    raise SystemExit(main())
