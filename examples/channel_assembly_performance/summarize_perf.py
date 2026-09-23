#!/usr/bin/env python3
"""Count fixed-period perf samples whose call stacks include planar execution.

Usage: summarize_perf.py perf.stacks
Only use a trace with zero lost samples. Unresolved libc leaf addresses remain
unresolved; this script does not rename them to memcpy based on expectation.
"""
import collections
import json
import re
import sys

self_counts = collections.Counter()
inclusive = collections.Counter()
rows = selected = 0
for block in open(sys.argv[1]).read().split('\n\n'):
    if 'cpu-clock:u:' not in block:
        continue
    rows += 1
    frames = []
    for line in block.splitlines()[1:]:
        found = re.match(r'\s*[0-9a-f]+\s+(.*)', line)
        if not found:
            continue
        frame = found.group(1).rsplit(' (', 1)[0]
        frame = re.sub(r'\+0x[0-9a-f]+$', '', frame)
        frames.append(frame)
    if not any('execute_planar' in name or 'copy_channel_piece' in name for name in frames):
        continue
    selected += 1
    if frames:
        self_counts[frames[0]] += 1
    for name in set(frames):
        inclusive[name] += 1

def report(counts):
    return [(name, count, round(count / selected * 100, 3)) for name, count in counts.most_common(15)] if selected else []
print(json.dumps({'total_samples': rows, 'execution_samples': selected,
                  'self': report(self_counts), 'inclusive': report(inclusive)}, indent=2))
