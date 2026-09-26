#!/usr/bin/env python3
"""Serialized, verified ABBA before/after comparison for the alpha driver.

Use two builds with identical flags. Both drivers must support the final
managed[off|on] argument. The script takes the shared POSIX performance lock,
rejects visible compiler activity, preserves commands and all latency samples,
and records binary hashes. It does not control machine temperature/frequency
or unrelated workloads. No third-party Python modules are required.
"""
from __future__ import annotations

import argparse
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import statistics
import subprocess
import sys
from datetime import datetime, timezone


def cases(suite: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []

    def add(**changes: str) -> None:
        case = dict(size='512', member='associate', storage='tiled', request='full',
                    algorithm='simd', dtype='f32', profile='strict', layout='auto',
                    distribution='mixed', managed='off')
        case.update(changes)
        if case not in result:
            result.append(case)

    for member in ('associate', 'unassociate'):
        for dtype in ('f32', 'f64'):
            for algorithm in ('scalar', 'simd'):
                add(member=member, dtype=dtype, algorithm=algorithm)
    if suite == 'core':
        return result
    for member in ('associate', 'unassociate'):
        for dtype in ('f32', 'f64'):
            add(member=member, dtype=dtype, managed='on')
            add(member=member, dtype=dtype, algorithm='auto')
        for storage in ('generic', 'continuous', 'tiled'):
            add(size='130', member=member, storage=storage, request='roi')
        add(size='17', member=member)
        add(size='2048', member=member, storage='continuous')
    for distribution in ('half', 'opaque', 'small'):
        add(member='unassociate', distribution=distribution)
    for layout in ('view', 'materialize'):
        add(member='set', layout=layout, algorithm='auto')
        add(member='set', layout=layout, algorithm='auto', managed='on')
    for member in ('extract', 'remove', 'opaque'):
        add(member=member, algorithm='auto')
    add(size='17', algorithm='reference', managed='on')
    add(size='17', member='unassociate', algorithm='reference', managed='on')
    if suite == 'latency':
        return [case for case in result if case['request'] == 'roi' or
                (case['size'] == '17' and case['algorithm'] != 'reference') or
                case['member'] in ('extract', 'remove', 'opaque')]
    return result


def compilers(allow_stopped: bool = False) -> list[str]:
    lines = subprocess.check_output(['ps', '-A', '-o', 'stat=', '-o', 'comm='], text=True).splitlines()
    name_pattern = re.compile(
        r'(?:clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+|cc1|cc1plus|ld(?:\.lld)?)'
        r'(?:-?[0-9]+(?:\.[0-9]+)*)?')
    result = []
    for line in lines:
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        state, command = fields
        if name_pattern.fullmatch(Path(command).name) and not (allow_stopped and 'T' in state):
            result.append(line.strip())
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='new directory; existing data will not be overwritten')
    parser.add_argument('--suite', choices=['core', 'extended', 'latency'], default='extended')
    parser.add_argument('--repetitions', type=int, default=11)
    parser.add_argument('--workers', type=int, default=1)
    parser.add_argument('--lock', type=Path, default=Path('/tmp/photospider-performance.lock'))
    parser.add_argument('--cpu', type=int,
                        help='pin via os.sched_setaffinity where supported')
    parser.add_argument('--allow-stopped-compilers', action='store_true',
                        help='permit only SIGSTOP-paused compilers; records their snapshot')
    parser.add_argument('--launcher', default='',
                        help='optional prefix, e.g. "cpuset -l 0" on FreeBSD')
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 10000 or not 1 <= args.workers <= 256:
        parser.error('repetitions must be 1..10000; workers must be 1..256')
    binaries = {'before': args.before.resolve(), 'after': args.after.resolve()}
    for binary in binaries.values():
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f'not an executable file: {binary}')
    if args.output.exists():
        parser.error('output directory already exists; choose a new directory')
    args.output.mkdir(parents=True)
    if args.cpu is not None:
        if not hasattr(os, 'sched_setaffinity'):
            parser.error('use --launcher for CPU affinity on this platform')
        os.sched_setaffinity(0, {args.cpu})
    with args.lock.open('a') as lock:
        # A busy performance lock is a visible failure, not a silent queue.
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        active = compilers(args.allow_stopped_compilers)
        if active:
            raise RuntimeError(f'compiler/linker activity detected: {active}')
        metadata = dict(started_utc=datetime.now(timezone.utc).isoformat(),
                        platform=platform.platform(), python=sys.version,
                        paused_build_processes=compilers() if args.allow_stopped_compilers else [],
                        command=sys.argv, order=['before', 'after', 'after', 'before'],
                        binaries={key: {'path': str(p), 'sha256': sha256(p)}
                                  for key, p in binaries.items()},
                        affinity=sorted(os.sched_getaffinity(0))
                        if hasattr(os, 'sched_getaffinity') else None)
        (args.output / 'environment.json').write_text(json.dumps(metadata, indent=2))
        rows: list[dict[str, str]] = []
        with (args.output / 'commands.log').open('w') as log, \
             (args.output / 'raw.csv').open('w', newline='') as output:
            writer = None
            for index, case in enumerate(cases(args.suite)):
                for repeat, variant in enumerate(metadata['order']):
                    active = compilers(args.allow_stopped_compilers)
                    if active:
                        raise RuntimeError(f'compiler/linker appeared during sampling: {active}')
                    command = [*shlex.split(args.launcher), str(binaries[variant]),
                               case['size'], case['member'], case['storage'], case['request'],
                               case['algorithm'], case['dtype'], str(args.repetitions),
                               case['profile'], case['layout'], str(args.workers),
                               case['distribution'], case['managed']]
                    process = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE, check=False)
                    log.write('$ ' + shlex.join(command) + '\n' + process.stderr +
                              process.stdout + f'EXIT={process.returncode}\n\n')
                    log.flush()
                    if process.returncode:
                        raise RuntimeError(f'case {index}/{variant} failed; see commands.log')
                    parsed = list(csv.DictReader(process.stdout.splitlines()))
                    if len(parsed) != 1:
                        raise RuntimeError('driver did not produce one validated CSV record')
                    row = dict(case=str(index), position=str(repeat), variant=variant, **parsed[0])
                    if row.get('managed') != case['managed']:
                        raise RuntimeError('driver lacks requested managed-resources instrumentation')
                    if writer is None:
                        writer = csv.DictWriter(output, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    output.flush()
                    rows.append(row)
                print(f'validated case {index + 1}/{len(cases(args.suite))}', file=sys.stderr)
        summary = []
        for index, case in enumerate(cases(args.suite)):
            group = [row for row in rows if row['case'] == str(index)]
            selected = {v: [r for r in group if r['variant'] == v] for v in binaries}
            medians = {v: statistics.median(float(r['p50_us']) for r in s)
                       for v, s in selected.items()}
            entry = dict(case=str(index), **case,
                         before_p50_us=medians['before'], after_p50_us=medians['after'],
                         speedup=medians['before'] / medians['after'],
                         before_process_p50s='/'.join(r['p50_us'] for r in selected['before']),
                         after_process_p50s='/'.join(r['p50_us'] for r in selected['after']),
                         issued_work_equal=len({r['issued_work'] for r in group}) == 1)
            summary.append(entry)
        with (args.output / 'summary.csv').open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=list(summary[0]))
            writer.writeheader()
            writer.writerows(summary)
        metadata['finished_utc'] = datetime.now(timezone.utc).isoformat()
        metadata['validated_processes'] = len(rows)
        (args.output / 'environment.json').write_text(json.dumps(metadata, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
