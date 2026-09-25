#!/usr/bin/env python3
"""Run validated FMT-04/05 cases and aggregate CSV; no optional Python packages.

The default suite is a smoke test. --matrix includes large cases and reference
arithmetic; its runtime is intentionally not bounded by this script. Every
child command and its stderr are preserved in --log. Nonzero exit is an error,
not a missing measurement; named unsupported profiles are selected explicitly.
"""
from __future__ import annotations
import argparse
import csv
import itertools
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--matrix', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('alpha-results.csv'))
    parser.add_argument('--log', type=Path, default=Path('alpha-results.log'))
    parser.add_argument('--profile', default='strict')
    parser.add_argument('--workers', type=int, default=1)
    parser.add_argument('--managed', choices=['off', 'on'], default='off')
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error('binary does not exist')
    sizes = [1, 17, 130, 512, 2048] if args.matrix else [17, 130]
    cases = []
    for size, storage, member in itertools.product(sizes, ['generic', 'continuous', 'tiled'],
            ['associate', 'unassociate', 'set', 'extract', 'remove', 'opaque']):
        algorithms = ['scalar', 'simd', 'reference'] if member in ['associate', 'unassociate'] else ['auto']
        for algorithm in algorithms:
            # Reference large-image runs are available via the binary, but
            # default matrix avoids making the exact oracle the dominant task.
            if algorithm == 'reference' and size > 130:
                continue
            for dtype in (['f32', 'f64'] if args.matrix else ['f32']):
                for request in (['full', 'red', 'roi'] if args.matrix else ['full']):
                    cases.append([str(size), member, storage, request, algorithm, dtype,
                                  str(args.repetitions), args.profile, 'auto', str(args.workers), 'mixed', args.managed])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w', newline='') as output, args.log.open('w') as log:
        writer = None
        for index, case in enumerate(cases, 1):
            command = [str(args.binary.resolve()), *case]
            print(f'[{index}/{len(cases)}] {" ".join(case)}', file=sys.stderr, flush=True)
            process = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            log.write('$ ' + ' '.join(command) + '\n' + process.stderr + process.stdout + '\n')
            log.flush()
            if process.returncode:
                print(f'Failed with status {process.returncode}; see {args.log}', file=sys.stderr)
                return process.returncode
            rows = list(csv.DictReader(process.stdout.splitlines()))
            if len(rows) != 1:
                raise RuntimeError('benchmark did not produce exactly one CSV result')
            if writer is None:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
            writer.writerow(rows[0]); output.flush()
    print(f'{len(cases)} validated measurements written to {args.output}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
