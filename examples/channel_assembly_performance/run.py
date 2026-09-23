#!/usr/bin/env python3
"""Serial FMT-02 public-workflow benchmarks with byte oracles in each process."""
import csv
import pathlib
import platform
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=True)
profile = 'accelerated_apple_silicon' if platform.machine() in ('arm64', 'aarch64') else 'accelerated_x86_64'
cases = []
for member in ('A', 'B'):
    for storage in ('continuous', 'tiled'):
        cases.append((128, member, storage, 'full', 'materialize', 31, 'strict'))
        cases.append((4096, member, storage, 'full', 'materialize', 9, 'strict'))
    for request in ('one', 'roi'):
        cases.append((4096, member, 'tiled', request, 'materialize', 31 if request == 'roi' else 9, 'strict'))
    cases.append((4096, member, 'tiled', 'full', 'auto', 9, 'strict'))
    cases.append((4096, member, 'tiled', 'full', 'materialize', 9, profile))
cases.extend([(128, 'C', 'tiled', 'full', 'materialize', 31, 'strict'),
              (4096, 'C', 'tiled', 'full', 'materialize', 9, 'strict'),
              (4096, 'C', 'tiled', 'roi', 'auto', 31, 'strict')])
for policy in ('view', 'auto', 'materialize'):
    cases.append((4096, 'view', 'tiled', 'full', policy, 9, 'strict'))
for request in ('one', 'roi'):
    cases.append((4096, 'view', 'tiled', request, 'view', 31, 'strict'))
rows = []
for case in cases:
    name = '-'.join(map(str, case))
    with (output / f'{name}.log').open('w') as log:
        result = subprocess.run([str(binary), *map(str, case)], stdout=subprocess.PIPE, stderr=log, text=True, check=True)
    (output / f'{name}.csv').write_text(result.stdout)
    rows.extend(csv.DictReader(result.stdout.splitlines()))
    print(name, rows[-1]['p50_us'], flush=True)
with (output / 'summary.csv').open('w', newline='') as stream:
    writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)
