#!/usr/bin/env python3
"""Report cycle-weighted self/inclusive CPU samples in metadata evaluation or public execute stacks."""
import collections
import json
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
ids = {node.get('id'): node for node in root.iter() if node.get('id')}
def resolve(node):
    return ids[node.get('ref')] if node.get('ref') else node
self_counts = collections.Counter()
inclusive = collections.Counter()
total = 0
rows = 0
for row in root.iter('row'):
    stack = row.find('tagged-backtrace')
    if stack is None:
        continue
    stack = resolve(stack)
    names = [resolve(frame).get('name', '') for frame in stack.findall('frame')]
    if not any('ExecutionContext::execute' in name or 'metadata_internal' in name for name in names):
        continue
    weight = int(resolve(row.find('cycle-weight')).text)
    total += weight
    rows += 1
    if names:
        self_counts[names[0]] += weight
    for name in set(names):
        inclusive[name] += weight
result = {'rows': rows, 'cycles': total,
          'self': [(name, round(weight / total * 100, 3)) for name, weight in self_counts.most_common(20)],
          'inclusive': [(name, round(weight / total * 100, 3)) for name, weight in inclusive.most_common(30)]}
print(json.dumps(result, indent=2))
