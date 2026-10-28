#!/usr/bin/env python3
"""Attach product entry observers to split generated C functions."""

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

from hook_migrate import GeneratedSources, MigrationError


ENTRY = re.compile(r"^(\s*)debug_server_log_call_entry\(0x([0-9A-Fa-f]{8})u\);\s*$")


def prepare(source: Path, output: Path, expected_functions: int) -> dict:
    source, output = source.resolve(), output.resolve()
    if output.exists() or output == source or source in output.parents:
        raise MigrationError("output must be a new directory outside the input")
    indexed = GeneratedSources(source)
    if len(indexed.functions) != expected_functions:
        raise MigrationError("generated function count differs from the selected input")
    edits: dict[Path, list[tuple[int, str]]] = {}
    for address, function in indexed.functions.items():
        lines = indexed.lines[function.path]
        hits = [(i, ENTRY.match(lines[i])) for i in range(function.start, function.end)
                if ENTRY.match(lines[i])]
        if len(hits) != 1 or hits[0][1].group(2).upper() != address:
            raise MigrationError(f"function {address} needs one matching entry boundary")
        index, match = hits[0]
        if any("dc_game_entry(" in line for line in lines[function.start:function.end]):
            raise MigrationError(f"function {address} already has an entry observer")
        if any(re.match(r"block_[0-9A-Fa-f]{8}:", line) for line in lines[function.start:index]):
            raise MigrationError(f"function {address} entry follows a basic block")
        edits.setdefault(function.path, []).append(
            (index + 1, f"{match.group(1)}dc_game_entry(cpu, 0x{address}u);\n"))
    shutil.copytree(source, output)
    for path, insertions in edits.items():
        lines = indexed.lines[path][:]
        for index, text in sorted(insertions, reverse=True):
            lines.insert(index, text)
        lines.insert(1, '#include "dc_game_entry.h"\n')
        (output / path.relative_to(source)).write_text("".join(lines), encoding="utf-8", newline="")
    outputs = {str(path.relative_to(output)).replace("\\", "/"):
               hashlib.sha256(path.read_bytes()).hexdigest()
               for path in sorted(output.rglob("*")) if path.is_file()}
    return {"schema": 1, "function_count": len(indexed.functions),
            "shard_count": len(edits), "outputs": outputs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-functions", type=int, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = prepare(args.source, args.output, args.expected_functions)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError) as error:
        parser.exit(1, f"entry wiring: {error}\n")
    print(f"Wired {report['function_count']} function entries in {report['shard_count']} shards.")


if __name__ == "__main__":
    main()
