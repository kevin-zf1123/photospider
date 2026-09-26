#!/usr/bin/env python3
"""Serial, correctness-gated ABBA comparisons; Python standard library only.

Pass two separately built *revision-2 CLI* drivers. For an original kernel,
compile this revision's main.cpp against the original headers and library.
No golden result, clock sample or backend failure is silently skipped.
"""
import argparse
import csv
import datetime
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


def workloads(profile, repeats, include_4k=False):
    cases = []

    def add(name, size, curve, direction='decode', dtype='f64', numeric='strict',
            storage='tiled', coverage='full', mode='respect', corpus='palette',
            budget='unmanaged', tile=128):
        cases.append((name, [str(size), curve, direction, dtype, numeric, storage,
                             coverage, str(repeats), str(tile), '1', mode, corpus, budget]))

    for curve in ('pq', 'srgb'):
        for corpus in ('palette', 'sweep'):
            for numeric in ('strict', profile):
                add(f'{curve}-{corpus}-{numeric}', 8, curve, numeric=numeric, corpus=corpus)
    add('pq-f32-encode', 8, 'pq', direction='encode', dtype='f32', corpus='sweep')
    add('bt1886-f64', 8, 'bt1886', corpus='sweep')
    add('gamma2-f32-256', 256, 'power_gamma2', dtype='f32', storage='continuous', corpus='sweep')
    add('gamma2-f32-1024', 1024, 'power_gamma2', dtype='f32', storage='continuous', corpus='sweep')
    add('gamma2-f32-raw', 256, 'power_gamma2', dtype='f32', storage='continuous', mode='raw', corpus='sweep')
    add('gamma2-f64-sqrt', 256, 'power_gamma2', direction='encode', storage='continuous', corpus='sweep')
    add('alpha-256', 256, 'acescc', dtype='f32', coverage='alpha')
    add('alpha-1024', 1024, 'acescc', dtype='f32', coverage='alpha', tile=256)
    add('hlg-cross-tile-roi', 258, 'hlg_oetf', coverage='roi', tile=256, corpus='sweep')
    add('linear-identity', 256, 'linear', dtype='f32', storage='continuous')
    add('srgb-generic-r-only', 8, 'srgb', storage='generic', coverage='r', corpus='sweep')
    if include_4k:
        add('gamma2-f32-4096', 4096, 'power_gamma2', dtype='f32', numeric=profile,
            storage='continuous', corpus='sweep')
    return cases


def ensure_no_builds():
    proc = subprocess.run(['ps', '-axo', 'stat,comm'], capture_output=True, text=True, check=True)
    active = []
    for line in proc.stdout.splitlines()[1:]:
        fields = line.split(None, 1)
        if len(fields) != 2 or fields[0].startswith('Z'):
            continue
        name = Path(fields[1]).name
        if re.fullmatch(r'(?:clang(?:\+\+)?(?:-?\d+)?|cc|c\+\+|cc1plus|ninja|cmake)', name):
            active.append(line)
    if active:
        raise RuntimeError('Refusing measurements while build tools are active: ' + '; '.join(active))


def quantile(values, q):
    ordered = sorted(values)
    position = (len(ordered) - 1) * q
    low = int(position)
    return ordered[low] + (ordered[min(low + 1, len(ordered) - 1)] - ordered[low]) * (position - low)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--profile', choices=['x86_64', 'apple_silicon'], default=(
        'apple_silicon' if platform.system() == 'Darwin' and platform.machine() in ('arm64', 'aarch64') else 'x86_64'))
    parser.add_argument('--cpu', type=int, help='Linux affinity or FreeBSD cpuset; unsupported on macOS')
    parser.add_argument('--include-4k', action='store_true')
    parser.add_argument('--managed', action='store_true', help='Both inputs must contain the fixed planar budget service')
    parser.add_argument('--case', action='append', help='Run only a named case; may be repeated')
    parser.add_argument('--all-curves', action='store_true')
    parser.add_argument('--corpus-table', type=Path)
    parser.add_argument('--workers', type=int, default=1)
    parser.add_argument('--size', type=int)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 1000:
        parser.error('repeats must be in 1..1000')
    binaries = {'before': args.before.resolve(), 'after': args.after.resolve()}
    for binary in binaries.values():
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f'not an executable file: {binary}')
    prefix = []
    if args.cpu is not None:
        if args.cpu < 0:
            parser.error('cpu must be nonnegative')
        if hasattr(os, 'sched_setaffinity'):
            os.sched_setaffinity(0, {args.cpu})
        elif platform.system() == 'FreeBSD':
            prefix = ['cpuset', '-l', str(args.cpu)]
        else:
            parser.error('CPU pinning is not implemented for this OS; omit --cpu')
    args.output.mkdir(parents=True, exist_ok=False)
    meta = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'platform': platform.platform(), 'processor': platform.processor(),
            'python': sys.version, 'argv': sys.argv, 'pid': os.getpid(),
            'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
            'note': 'Shared-host observations; no claim of locked frequency, cache flushing or idle system.'}
    (args.output / 'environment.json').write_text(json.dumps(meta, indent=2))
    cases = workloads(args.profile, args.repeats, args.include_4k)
    if args.all_curves:
        cases = []
        for curve in ('linear', 'power_gamma', 'power_gamma2', 'srgb', 'bt709',
                      'bt2020', 'bt2020_10', 'bt2020_12', 'bt1886', 'pq',
                      'hlg_oetf', 'acescc', 'acescct'):
            for direction in ('encode', 'decode'):
                for dtype in ('f32', 'f64'):
                    for numeric in ('strict', args.profile):
                        name = f'{curve}-{direction}-{dtype}-{numeric}'
                        cases.append((name, ['8', curve, direction, dtype, numeric,
                            'tiled', 'full', str(args.repeats), '128', '1',
                            'respect', 'sweep', 'unmanaged']))
    if args.case:
        available = {name for name, _ in cases}
        unknown = set(args.case) - available
        if unknown:
            parser.error('unknown cases: ' + ','.join(sorted(unknown)))
        cases = [(name, c) for name, c in cases if name in args.case]
    runs, samples, comparisons = [], [], []
    for case, arguments in cases:
        arguments[9] = str(args.workers)
        if args.size is not None:
            arguments[0] = str(args.size)
        if args.corpus_table:
            arguments[-2] = 'file:' + str(args.corpus_table.resolve())
        if args.managed:
            arguments[-1] = 'managed'
        group = {'before': [], 'after': []}
        for order, variant in enumerate(('before', 'after', 'after', 'before')):
            ensure_no_builds()
            command = prefix + [str(binaries[variant])] + arguments
            started = time.monotonic()
            process = subprocess.run(command, capture_output=True, text=True, timeout=300)
            stem = f'{case}-{order}-{variant}'
            (args.output / (stem + '.stdout.csv')).write_text(process.stdout)
            (args.output / (stem + '.stderr.log')).write_text(process.stderr)
            entry = {'case': case, 'order': order, 'variant': variant, 'command': command,
                     'returncode': process.returncode, 'process_wall_seconds': time.monotonic() - started}
            runs.append(entry)
            (args.output / 'runs.json').write_text(json.dumps(runs, indent=2))
            if process.returncode:
                raise RuntimeError(f'{stem}: failed; raw stdout/stderr retained')
            rows = list(csv.DictReader(process.stdout.splitlines()))
            if len(rows) != 1 or int(rows[0]['verified_executions']) != args.repeats + 2:
                raise RuntimeError(f'{stem}: missing per-execution correctness gate')
            entry['summary'] = rows[0]
            seen = 0
            for line in process.stderr.splitlines():
                if not line.startswith('SAMPLE,'):
                    continue
                _, iteration, warmup, wall, callback, evaluated, math_calls, work = line.split(',')
                row = dict(case=case, variant=variant, order=order, iteration=int(iteration),
                           warmup=int(warmup), wall_us=float(wall), callback_us=float(callback),
                           evaluated=int(evaluated), strict_math_calls=int(math_calls), issued_work=int(work))
                if row['iteration'] != seen or row['warmup'] != int(seen < 2):
                    raise RuntimeError(f'{stem}: noncontiguous or mislabeled execution samples')
                samples.append(row)
                if not row['warmup']:
                    group[variant].append(row['wall_us'])
                seen += 1
            if seen != args.repeats + 2:
                raise RuntimeError(f'{stem}: incomplete sample trace')
        before, after = group['before'], group['after']
        row = {'case': case, 'n_per_variant': len(before),
               'before_median_us': statistics.median(before), 'after_median_us': statistics.median(after),
               'before_p95_us': quantile(before, .95), 'after_p95_us': quantile(after, .95),
               'speedup': statistics.median(before) / statistics.median(after)}
        comparisons.append(row)
        print(f'{case}: {row["before_median_us"]:.3f} -> {row["after_median_us"]:.3f} us; {row["speedup"]:.3f}x', flush=True)
    (args.output / 'runs.json').write_text(json.dumps(runs, indent=2))
    for filename, rows in [('samples.csv', samples), ('comparison.csv', comparisons)]:
        with (args.output / filename).open('w', newline='') as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    meta['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (args.output / 'environment.json').write_text(json.dumps(meta, indent=2))


if __name__ == '__main__':
    main()
