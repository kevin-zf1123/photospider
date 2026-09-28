"""Compare public CPU workflows, exact output bytes and child OS counters."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1 << 20), b''):
            result.update(block)
    return result.hexdigest()


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--runner', type=Path, required=True)
p.add_argument('--reference-runner', type=Path)
p.add_argument('--plugin', type=Path, required=True)
p.add_argument('--reference-plugin', type=Path, required=True)
p.add_argument('--reference-backend', choices=['cpu', 'cpu_tiled'], default='cpu')
p.add_argument('--reference-label', default='CPU Whole')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--sizes', default='128x128,1920x1080,4096x4096')
p.add_argument('--repeat', type=int, default=11)
p.add_argument('--workers', type=int, default=4)
p.add_argument('--rounds', type=int, default=2)
a = p.parse_args()
if min(a.repeat, a.workers, a.rounds) <= 0:
    p.error('repeat, workers and rounds must be positive')
a.out.mkdir(parents=True, exist_ok=True)
variants = [('reference', a.reference_plugin, a.reference_backend),
            ('candidate', a.plugin, 'cpu_tiled')]
with (a.out / 'comparison.jsonl').open('w') as report:
    for size in a.sizes.split(','):
        width, height = map(int, size.split('x'))
        expected = None
        for round_index in range(a.rounds):
            # Alternate ordering to expose drift between paired runs.
            order = variants if round_index % 2 == 0 else variants[::-1]
            for variant, plugin, backend in order:
                with tempfile.TemporaryDirectory(dir=a.out) as temporary:
                    temporary = Path(temporary)
                    output = temporary / 'result.f32'
                    runner = a.reference_runner if variant == 'reference' and a.reference_runner else a.runner
                    command = [str(runner), str(plugin), str(width), str(height),
                               'backend=' + backend, 'workers=' + str(a.workers),
                               'warmup=1', 'repeat=' + str(a.repeat),
                               'output=' + str(output)]
                    stdout, stderr = temporary / 'stdout', temporary / 'stderr'
                    with stdout.open('w') as out, stderr.open('w') as err:
                        process = subprocess.Popen(
                            command, stdout=out, stderr=err,
                            env={**os.environ, 'PIXELOE_CPU_SIMD': '1'})
                        _, status, usage = os.wait4(process.pid, 0)
                        process.returncode = os.waitstatus_to_exitcode(status)
                    if process.returncode:
                        raise RuntimeError(stderr.read_text())
                    actual = digest(output)
                    if expected is None:
                        expected = actual
                    identical = actual == expected
                    record = dict(
                        variant=variant, round=round_index, width=width, height=height,
                        command=command, stdout=stdout.read_text().strip(),
                        reference_label=a.reference_label, bitwise_reference=identical,
                        output_sha256=actual, child_user_seconds=usage.ru_utime,
                        child_system_seconds=usage.ru_stime,
                        voluntary_context_switches=usage.ru_nvcsw,
                        involuntary_context_switches=usage.ru_nivcsw,
                        minor_faults=usage.ru_minflt, major_faults=usage.ru_majflt,
                        maxrss_native_units=usage.ru_maxrss, platform=sys.platform,
                        counter_scope='complete child including warmup and fixture IO')
                    line = json.dumps(record)
                    report.write(line + '\n')
                    report.flush()
                    print(line, flush=True)
                    if not identical:
                        raise AssertionError('CPU tiled output differs from reference')
