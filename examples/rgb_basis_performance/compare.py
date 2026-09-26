#!/usr/bin/env python3
"""Serial, same-profile comparison of two FMT-10 executable builds (stdlib only)."""
import argparse
import csv
import datetime
import fcntl
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--profile', required=True,
                        choices=['strict', 'accelerated_apple_silicon',
                                 'accelerated_x86_64'])
    parser.add_argument('--size', type=int, default=1024)
    parser.add_argument('--repeats', type=int, default=7)
    parser.add_argument('--cpu', type=int, help='Linux/FreeBSD affinity, not macOS')
    parser.add_argument('--lock', type=Path,
                        default=Path('/tmp/photospider-performance.lock'))
    args = parser.parse_args()
    if args.size < 3 or args.size > 4096 or not 3 <= args.repeats <= 100:
        parser.error('size must be 3..4096; repeats must be 3..100')
    for path in (args.baseline, args.candidate):
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error('executable not found: ' + str(path))
    # Never overwrite an earlier measurement set.
    args.out.mkdir(parents=True, exist_ok=False)
    prefix = []
    if args.cpu is not None:
        if args.cpu < 0:
            parser.error('CPU index must be nonnegative')
        system = platform.system()
        if system == 'Linux':
            prefix = ['taskset', '-c', str(args.cpu)]
        elif system == 'FreeBSD':
            prefix = ['cpuset', '-l', str(args.cpu)]
        else:
            parser.error('--cpu is supported only on Linux/FreeBSD')
    environment = {
        'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'platform': platform.platform(), 'machine': platform.machine(),
        'python': sys.version, 'arguments': {k: str(v) for k, v in vars(args).items()},
        'measurement': 'public execute wall time; no profiler; each process has '
                       'a small strict gate, not a full-image untimed warmup; '
                       'all iterations retained; steady summary drops index 0',
    }
    (args.out / 'environment.json').write_text(json.dumps(environment, indent=2))
    records = []
    failed = False
    with args.lock.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for index, (dtype, member) in enumerate(
                (dtype, member) for dtype in ['f32', 'f64'] for member in 'abcd'):
            pair = [('baseline', args.baseline), ('candidate', args.candidate)]
            if index % 2:
                pair.reverse()
            for label, executable in pair:
                stem = f'{dtype}-{member}-{label}'
                command = prefix + [str(executable.resolve()), str(args.size),
                                    dtype, member, args.profile, 'full',
                                    str(args.repeats), 'planar', '128', '1',
                                    'materialize', 'respect']
                with (args.out / (stem + '.csv')).open('w') as stdout, \
                        (args.out / (stem + '.stderr.txt')).open('w') as stderr:
                    code = subprocess.call(command, stdout=stdout, stderr=stderr)
                record = {'case': f'{dtype}-{member}', 'variant': label,
                          'command': command, 'exit': code}
                if code == 0:
                    with (args.out / (stem + '.csv')).open() as stream:
                        values = [float(row['execute_ms']) for row in csv.DictReader(stream)]
                    if len(values) != args.repeats:
                        raise RuntimeError('unexpected iteration count: ' + stem)
                    record.update(median_all_ms=statistics.median(values),
                                  median_steady_ms=statistics.median(values[1:]),
                                  min_steady_ms=min(values[1:]),
                                  max_steady_ms=max(values[1:]))
                else:
                    failed = True
                records.append(record)
                with (args.out / 'runs.jsonl').open('a') as stream:
                    stream.write(json.dumps(record) + '\n')
                print(stem, 'exit', code, record.get('median_steady_ms'), flush=True)
    (args.out / 'summary.json').write_text(json.dumps(records, indent=2))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
