#!/usr/bin/env python3
"""Record Xcode CPU Profiler samples and summarize execution-only cycle weights.

Usage: profile.py binary output.trace [benchmark arguments...]
The trace destination must be new. Profiler timings are not benchmark timings.
"""
import json
import pathlib
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
if output.exists():
    raise RuntimeError('Trace destination already exists')
args = sys.argv[3:] or ['4096', 'A', 'tiled', 'full', 'materialize', '1000', 'strict']
prefix = ['/usr/bin/arch', '-arm64', 'xcrun', 'xctrace']
log = output.with_suffix('.record.log')
with log.open('w') as stream:
    result = subprocess.run(prefix + ['record', '--template', 'Blank',
        '--instrument', 'CPU Profiler', '--time-limit', '10s', '--output', str(output),
        '--launch', '--', str(binary)] + args, stdout=stream, stderr=stream, timeout=90)
text = log.read_text()
if (result.returncode not in (0, 54) or 'Recording completed.' not in text or
        'Output file saved as:' not in text or 'Run issues were detected' in text or '[Error]' in text):
    raise RuntimeError(f'Recording failed ({result.returncode}): {log}')
xml = output.with_suffix('.xml')
subprocess.run(prefix + ['export', '--input', str(output), '--xpath',
    '/trace-toc/run[@number="1"]/data/table[@schema="cpu-profile"]', '--output', str(xml)], check=True)
summary_script = pathlib.Path(__file__).resolve().parents[1] / 'channel_extraction_performance' / 'summarize_trace.py'
summary = subprocess.check_output([sys.executable, str(summary_script), str(xml)], text=True)
if not json.loads(summary)['rows']:
    raise RuntimeError('No execution CPU samples')
output.with_suffix('.hotspots.json').write_text(summary)
print(summary)
