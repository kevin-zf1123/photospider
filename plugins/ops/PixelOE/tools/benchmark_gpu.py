"""Native GPU workflow timings, child OS counters and exact revision comparison."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--runner', type=Path, required=True)
p.add_argument('--plugin', type=Path, required=True)
p.add_argument('--reference-plugin', type=Path)
p.add_argument('--reference-operation')
p.add_argument('--backend', choices=['gpu', 'vulkan'], default='gpu')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--sizes', default='128x128,1920x1080,4096x4096')
p.add_argument('--repeat', type=int, default=5)
p.add_argument('--parameter', action='append', default=[])
a = p.parse_args()
if a.repeat < 1:
    p.error('repeat must be positive')
a.out.parent.mkdir(parents=True, exist_ok=True)
operation = 'pixeloe.pixelize_' + ('vulkan' if a.backend == 'vulkan' else 'metal') + '_native_fp32'
variants = [('current', a.plugin, operation)]
if a.reference_plugin:
    variants.insert(0, ('reference', a.reference_plugin, a.reference_operation or operation))
with a.out.open('w') as report:
    for size in a.sizes.split(','):
        width, height = map(int, size.split('x'))
        previous = None
        for variant, plugin, operation in variants:
            with tempfile.TemporaryDirectory(dir=a.out.parent) as temporary:
                temporary = Path(temporary)
                output, log = temporary / 'image.f32', temporary / 'process.txt'
                command = [str(a.runner), str(plugin), str(width), str(height),
                           'backend=' + a.backend, 'operation=' + operation, 'warmup=1',
                           'repeat=' + str(a.repeat), 'output=' + str(output)]
                command += a.parameter
                with log.open('w') as stream:
                    process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
                    _, status, usage = os.wait4(process.pid, 0)
                    process.returncode = os.waitstatus_to_exitcode(status)
                if process.returncode:
                    raise RuntimeError(log.read_text())
                with output.open('rb') as stream:
                    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                identical = previous is None or previous == digest
                previous = digest
                # The output file is temporary; omit that transient path from
                # the reproducible command and retain the checked digest.
                command = [item for item in command if not item.startswith('output=')]
                record = dict(variant=variant, width=width, height=height, command=command,
                              workflow=log.read_text().strip(), output_sha256=digest,
                              bitwise_reference=identical, child_user_seconds=usage.ru_utime,
                              child_system_seconds=usage.ru_stime,
                              voluntary_context_switches=usage.ru_nvcsw,
                              involuntary_context_switches=usage.ru_nivcsw,
                              minor_faults=usage.ru_minflt, major_faults=usage.ru_majflt,
                              maxrss_native_units=usage.ru_maxrss,
                              counter_scope='complete process, including warmup and fixture IO')
                line = json.dumps(record)
                report.write(line + '\n')
                report.flush()
                print(line, flush=True)
                if not identical:
                    raise AssertionError('GPU revision comparison changed output bits')
