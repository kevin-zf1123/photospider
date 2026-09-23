#!/usr/bin/env python3
"""Summarize exported System Trace XML; syscall/VM rows filter execute stacks.

Usage: summarize_system_trace.py path/prefix
Expects prefix-{syscall,virtual-memory,thread-state}.xml. Durations are sums of
recorded intervals and may overlap; they are not a wall-time decomposition.
Thread states cover the target main thread including setup and verification.
"""
import collections
import json
import pathlib
import sys
import xml.etree.ElementTree as ET

prefix = pathlib.Path(sys.argv[1])
result = {}
for schema, field in [('syscall', 'syscall'), ('virtual-memory', 'vm-op'), ('thread-state', 'thread-state')]:
    cache = {}
    counts = collections.Counter()
    durations = collections.Counter()
    cpu = collections.Counter()
    waiting = collections.Counter()
    total = matched = 0
    keep = {'frame', 'tagged-backtrace', 'duration', 'duration-on-core',
            'duration-waiting', 'syscall', 'vm-op', 'thread-state', 'thread'}
    def value(node):
        if node is None:
            return ('', '', ())
        return cache.get(node.get('ref'), (node.text or '', node.get('fmt', ''), ()))
    for event, node in ET.iterparse(str(prefix) + '-' + schema + '.xml', events=('end',)):
        if node.get('id') and node.tag in keep:
            if node.tag == 'frame':
                cache[node.get('id')] = ('', '', (node.get('name', ''),))
            elif node.tag == 'tagged-backtrace':
                frames = tuple(name for child in node if child.tag == 'frame'
                               for name in cache.get(child.get('ref') or child.get('id'), ('', '', ()))[2])
                cache[node.get('id')] = ('', '', frames)
            else:
                cache[node.get('id')] = (node.text or '', node.get('fmt', ''), ())
        if node.tag != 'row':
            continue
        total += 1
        if schema == 'thread-state':
            selected = 'Main Thread' in value(node.find('thread'))[1]
        else:
            backtrace = node.find('tagged-backtrace')
            stack = cache.get(backtrace.get('ref') or backtrace.get('id'), ('', '', ()))[2] if backtrace is not None else ()
            selected = any('execute_planar' in frame or 'copy_channel_piece' in frame for frame in stack)
        if selected:
            matched += 1
            record = value(node.find(field))
            name = record[1] or record[0]
            counts[name] += 1
            def number(tag):
                text = value(node.find(tag))[0]
                return int(text) if text else 0
            durations[name] += number('duration')
            cpu[name] += number('duration-on-core')
            waiting[name] += number('duration-waiting')
        node.clear()
    result[schema] = {'total_rows': total, 'selected_rows': matched,
                      'scope': 'main thread, all stages' if schema == 'thread-state' else 'execute_planar/copy_channel_piece stacks',
                      'events': [{'name': name, 'count': count, 'duration_ms': round(durations[name] / 1e6, 3),
                                  'cpu_ms': round(cpu[name] / 1e6, 3), 'wait_ms': round(waiting[name] / 1e6, 3)}
                                 for name, count in counts.most_common(20)]}
print(json.dumps(result, indent=2))
