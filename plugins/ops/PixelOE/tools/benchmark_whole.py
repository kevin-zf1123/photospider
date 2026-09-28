"""Controlled installed-workflow comparison with exact output and OS counters."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--sizes', default='128x128,1920x1080,4096x4096')
p.add_argument('--repeat', type=int, default=3)
p.add_argument('--workers', type=int, default=4)
p.add_argument('--reference-label', default='supplied-snapshot')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)

def equal(left, right):
    with left.open('rb') as x, right.open('rb') as y:
        while True:
            b = x.read(1 << 20)
            if b != y.read(1 << 20):
                return False
            if not b:
                return True

with (a.out / 'whole.jsonl').open('w') as report:
    for size in a.sizes.split(','):
        w, h = map(int, size.split('x'))
        with tempfile.TemporaryDirectory(dir=a.out) as temporary:
            temporary = Path(temporary)
            baseline_output = temporary / 'baseline.f32'
            for variant, build, workers in [('baseline', a.baseline, None),
                                             ('whole-1', a.build, 1),
                                             ('whole-many', a.build, a.workers)]:
                output = baseline_output if variant == 'baseline' else temporary / (variant + '.f32')
                cmd = [str(build / 'pixeloe_workflow'),
                       str(build / 'libphotospider_pixeloe.so'), str(w), str(h),
                       'warmup=1', 'repeat=' + str(a.repeat), 'output=' + str(output)]
                if workers is not None:
                    cmd += ['workers=' + str(workers)]
                # Files avoid pipe-capacity waits while wait4 collects this
                # exact child, including warmup and fixture/output I/O.
                stdout = temporary / 'stdout.txt'
                stderr = temporary / 'stderr.txt'
                with stdout.open('w') as out, stderr.open('w') as err:
                    process = subprocess.Popen(cmd, stdout=out, stderr=err,
                                               env={**os.environ, 'PIXELOE_CPU_SIMD': '1'})
                    _, status, usage = os.wait4(process.pid, 0)
                    process.returncode = os.waitstatus_to_exitcode(status)
                if process.returncode:
                    raise RuntimeError(stderr.read_text())
                identical = variant == 'baseline' or equal(baseline_output, output)
                record = dict(variant=variant, width=w, height=h, command=cmd,
                              stdout=stdout.read_text().strip(), bitwise_reference=identical,
                              reference_label=a.reference_label,
                              child_user_seconds=usage.ru_utime, child_system_seconds=usage.ru_stime,
                              voluntary_context_switches=usage.ru_nvcsw,
                              involuntary_context_switches=usage.ru_nivcsw,
                              minor_faults=usage.ru_minflt, major_faults=usage.ru_majflt,
                              maxrss_native_units=usage.ru_maxrss, platform=sys.platform,
                              counter_scope='complete child including warmup and fixture IO')
                report.write(json.dumps(record) + '\n')
                report.flush()
                print(json.dumps(record), flush=True)
                if not identical:
                    raise AssertionError('Whole result differs from supplied baseline')
