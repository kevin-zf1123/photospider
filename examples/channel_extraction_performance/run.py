#!/usr/bin/env python3
"""Run the fixed native FMT-01 matrix serially; save stdout and setup diagnostics."""
import csv
import pathlib
import platform
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
arm_host = platform.machine().lower() in ('arm64', 'aarch64')
if sys.platform == 'darwin':
    arm_host = subprocess.check_output(
        ['sysctl', '-n', 'hw.optional.arm64'], text=True).strip() == '1'
native_profile = 'accelerated_apple_silicon' if arm_host else 'accelerated_x86_64'
cases = [(128, 'continuous', 'fp32', 'all', 31, 'strict', 'full'),
         (128, 'tiled', 'fp32', 'all', 31, 'strict', 'full'),
         (4096, 'continuous', 'fp32', 'all', 9, 'strict', 'full'),
         (4096, 'tiled', 'fp32', 'all', 9, 'strict', 'full'),
         (4096, 'tiled', 'fp32', 'all', 31, 'strict', 'roi'),
         (128, 'continuous', 'fp64', 'all', 31, 'strict', 'full'),
         (4096, 'tiled', 'u8', 'all', 9, 'strict', 'full'),
         (4096, 'tiled', 'fp64', 'all', 9, 'strict', 'full'),
         (4096, 'tiled', 'fp32', 'materialize', 9, native_profile, 'full')]
rows = []
for case in cases:
    name = '-'.join(map(str, case))
    print(name, flush=True)
    result = subprocess.run([str(binary), *map(str, case)], capture_output=True, text=True)
    (out / (name + '.csv')).write_text(result.stdout)
    (out / (name + '.log')).write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr)
    parsed = list(csv.DictReader(result.stdout.splitlines()))
    rows.extend(parsed)
    with (out / 'summary.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for row in parsed:
        print(' ', row['layout'], row['p50_us'], 'us', flush=True)
