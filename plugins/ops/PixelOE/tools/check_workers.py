"""Replay option profiles through installed one/many-worker workflows."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--profiles', type=Path, required=True)
p.add_argument('--input', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--width', type=int, default=31)
p.add_argument('--height', type=int, default=25)
p.add_argument('--workers', type=int, default=4)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
records = []
for profile in json.loads(a.profiles.read_text()):
    output = a.out / (profile['name'] + '.f32')
    reference = a.out / (profile['name'] + '-one.f32')
    commands = []
    for workers, dest in [(1, reference), (a.workers, output)]:
        cmd = [str(a.build / 'pixeloe_workflow'),
               str(a.build / 'libphotospider_pixeloe.so'), str(a.width), str(a.height),
               'workers=' + str(workers), 'input=' + str(a.input), 'output=' + str(dest)]
        cmd += [f'{k}={str(v).lower() if isinstance(v, bool) else v}'
                for k, v in profile['options'].items()]
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        commands.append(dict(command=cmd, stdout=result.stdout.strip()))
    identical = output.read_bytes() == reference.read_bytes()
    record = dict(name=profile['name'], options=profile['options'],
                  bitwise_equal=identical, runs=commands)
    records.append(record)
    (a.out / 'workers.json').write_text(json.dumps(records, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    if not identical:
        raise AssertionError(profile['name'] + ': worker count changes output')
