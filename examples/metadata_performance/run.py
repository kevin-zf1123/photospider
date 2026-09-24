#!/usr/bin/env python3
"""Run representative FMT-08 workloads serially; outputs are JSONL."""
import json
import pathlib
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
cases = [
    (512, 'generic', 'materialize', 'patch', 'full', 4),
    (512, 'generic', 'view', 'patch', 'full', 4),
    (512, 'generic', 'materialize', 'patch', 'one', 4),
    (512, 'generic', 'materialize', 'patch', 'roi', 4),
    (4096, 'tiled', 'auto', 'patch', 'full', 4),
    (4096, 'tiled', 'view', 'patch', 'full', 4),
    (4096, 'tiled', 'materialize', 'patch', 'full', 4),
    (4096, 'continuous', 'view', 'patch', 'full', 4),
    (4096, 'continuous', 'materialize', 'patch', 'full', 4),
    (4096, 'tiled', 'materialize', 'patch', 'one', 4),
    (4096, 'tiled', 'materialize', 'patch', 'roi', 4),
]
for edit in ('patch', 'replace', 'cascade'):
    for layout in ('view', 'materialize'):
        cases.append((512, 'tiled', layout, edit, 'full', 32))
with out.open('w') as stream:
    for size, storage, layout, edit, request, channels in cases:
        command = [str(binary), str(size), storage, layout, edit, request, '7', str(channels)]
        result = subprocess.run(command, text=True, capture_output=True, check=True)
        row = json.loads(result.stdout)
        stream.write(json.dumps(row) + '\n')
        stream.flush()
        print(size, storage, layout, edit, request, channels, row['public_p50_us'], flush=True)
