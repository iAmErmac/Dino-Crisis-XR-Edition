#!/usr/bin/env python3
"""Separate temporary pose restoration from shared guest branch targets."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def rewrite(text):
    pattern = re.compile(r'^    dc_pose_call_end\(cpu, 0x([0-9A-F]{8})u\);\r?\n', re.M)
    addresses = pattern.findall(text)
    if len(addresses) != len(set(addresses)):
        raise ValueError('pose return identities must be unique within a shard')
    for pc in addresses:
        label = f'block_{pc}:\n'
        case = f'case 0x{pc}u: goto block_{pc};'
        if text.count(label) != 1 or text.count(case) != 1:
            raise ValueError(f'pose return {pc} requires one block and one resume case')
        end = f'    dc_pose_call_end(cpu, 0x{pc}u);\n'
        if text.count(end) != 1 or text.index(end) < text.index(label):
            raise ValueError(f'pose return {pc} has an unexpected restore location')
        text = text.replace(end, '', 1)
        text = text.replace(label, f'dc_pose_return_{pc}:\n{end}{label}', 1)
        text = text.replace(case, f'case 0x{pc}u: goto dc_pose_return_{pc};', 1)
    return text, addresses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if output.exists() or output == source or source in output.parents:
        parser.error('output must be a new independent directory')
    if any((parent / '.git').exists() for parent in (output, *output.parents)):
        parser.error('generated output must remain outside Git checkouts')
    files, restored = {}, []
    for path in source.iterdir():
        if not path.is_file():
            parser.error('source must contain only generated files')
        data = path.read_bytes()
        if re.search(r'_full_\d+\.c$', path.name):
            text, pcs = rewrite(data.decode('utf-8').replace('\r\n', '\n'))
            if pcs:
                data = text.encode('utf-8')
                restored.extend(pcs)
        files[path.name] = data
    if not restored:
        parser.error('no pose returns found')
    output.mkdir(parents=True)
    hashes = {}
    for name, data in files.items():
        (output / name).write_bytes(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
    args.report.write_text(json.dumps({'pose_returns': restored, 'outputs': hashes}, indent=2)+'\n', encoding='utf-8')
    print(f'Separated {len(restored)} pose return edges')


if __name__ == '__main__':
    main()
