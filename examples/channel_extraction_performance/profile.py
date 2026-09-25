#!/usr/bin/env python3
"""Record native CPU Profiler cycles and export execution-only hotspot weights.

Use the CPU instrument alone to avoid ancillary macOS log-stream failures.
Require completed recording plus exported, nonempty execution samples.
Setup/oracle stacks are excluded by summarize_trace.py.
"""
import json
import pathlib
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
if output.exists():
    raise RuntimeError('Use a fresh trace output name')
prefix = ['/usr/bin/arch', '-arm64', 'xcrun', 'xctrace']
with output.with_suffix('.record.log').open('w') as trace_log:
    recorded = subprocess.run(prefix + ['record', '--template', 'Blank', '--instrument', 'CPU Profiler',
        '--time-limit', '10s', '--output', str(output), '--launch', '--',
        str(binary), '4096', 'tiled', 'fp32', 'materialize', '1000', 'strict', 'full'],
        stdout=trace_log, stderr=trace_log, timeout=90)
print('xctrace exit:', recorded.returncode, flush=True)
log_text = output.with_suffix('.record.log').read_text()
# xctrace can return 54 after a completed, saved launch recording on this host.
# Accept only that observed code or zero, with explicit success and no issues.
if (recorded.returncode not in (0, 54) or
        'Recording completed.' not in log_text or
        'Output file saved as:' not in log_text or
        'Run issues were detected' in log_text or '[Error]' in log_text):
    raise RuntimeError('Recording failed; inspect ' + str(output.with_suffix('.record.log')))
xml = output.with_suffix('.xml')
subprocess.run(prefix + ['export', '--input', str(output), '--xpath',
    '/trace-toc/run[@number="1"]/data/table[@schema="cpu-profile"]',
    '--output', str(xml)], check=True)
summary = subprocess.check_output([sys.executable,
    str(pathlib.Path(__file__).with_name('summarize_trace.py')), str(xml)], text=True)
if not json.loads(summary)['rows']:
    raise RuntimeError('No execution CPU samples were exported')
output.with_suffix('.hotspots.json').write_text(summary)
print('Validated execution samples:', json.loads(summary)['rows'], flush=True)
