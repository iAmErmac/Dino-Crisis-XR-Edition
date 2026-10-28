#!/usr/bin/env python3
"""Describe validated generated-code edits by guest function and instruction."""

import argparse
import hashlib
import json
import re
from pathlib import Path

import posdiff


FUNCTION = re.compile(rb"^void func_([0-9A-Fa-f]{8})\(")
INSTRUCTION = re.compile(rb"/\* 0x([0-9A-Fa-f]{8}): 0x([0-9A-Fa-f]{8}) \*/")
CALL = re.compile(r"\bfunc_([0-9A-Fa-f]{8})\(cpu\)")


def describe(patch, source):
    files = posdiff.parse(str(patch))
    if len(files) != 1:
        raise ValueError("expected one generated source per hook patch")
    record = files[0]
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != record["sha"]:
        raise ValueError("generated source does not match the hook preimage hash")
    lines = posdiff.split_lines(data)
    if len(lines) != record["lines"]:
        raise ValueError("generated source line count does not match")

    owners = []
    owner = None
    instructions = []
    for index, line in enumerate(lines):
        match = FUNCTION.match(line)
        if match:
            owner = match[1].decode("ascii").upper()
        owners.append(owner)
        match = INSTRUCTION.search(line)
        if match:
            instructions.append((index, owner, match[1].decode("ascii").upper(),
                                 match[2].decode("ascii").upper()))

    edits = []
    previous_end = 0
    for index, operation in enumerate(record["ops"]):
        start = operation["at"]
        end = start + operation["drop"]
        if start < previous_end or end > len(lines):
            raise ValueError("invalid or overlapping edit range")
        previous_end = end
        owner = owners[start] if start < len(owners) else owners[-1]
        payload = b"".join(posdiff.build_line(item, lines, record["rel"])
                           for item in operation["add"])
        text = payload.decode("utf-8")
        prior = [entry for entry in instructions if entry[0] < start and entry[1] == owner]
        following = [entry for entry in instructions if entry[0] >= end and entry[1] == owner]
        inside = [entry for entry in instructions if start <= entry[0] < end]

        def site(entry):
            return {"pc": entry[2], "opcode": entry[3]}

        edits.append({
            "id": f"H{index:03d}",
            "function": owner,
            "old_line": start + 1,
            "removed_lines": operation["drop"],
            "inserted_lines": len(payload.splitlines()),
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
            "previous_instruction": site(prior[-1]) if prior else None,
            "next_instruction": site(following[0]) if following else None,
            "replaced_instructions": [site(entry) for entry in inside],
            "guest_calls": sorted(set(CALL.findall(text))),
            "changes_register_cache": "#define PSX_SPILL" in text,
            "classification": "unresolved",
        })

    return {
        "schema": 1,
        "source_sha256": digest,
        "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
        "source_lines": len(lines),
        "edit_count": len(edits),
        "note": "Neighboring instructions locate an old edit; they do not prove a new insertion boundary.",
        "edits": edits,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patch", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    try:
        result = describe(arguments.patch, arguments.source)
    except (ValueError, UnicodeError) as error:
        parser.exit(1, f"hook catalog: {error}\n")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {result['edit_count']} edits across {result['source_lines']} source lines.")


if __name__ == "__main__":
    main()
