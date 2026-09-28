"""Compare actual native Perlin calculator bits with the Fraction oracle.

Usage: python3 oracle/ops/generation/check_perlin_runtime.py --runner PATH
The runner selects arithmetic or public workflow execution. The GPU workflow
runner additionally requires actual native dispatch and rejects CPU fallback.
Resource and concurrency tests are separate.
"""
import argparse
import random
import subprocess
import json
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runner', required=True)
parser.add_argument('--tiled', action='store_true')
args = parser.parse_args()
import exact,rng
from fractions import Fraction
r=random.Random(90104)
cases=[]
for small in [False,True]:
    width=32 if small else 64
    dtype='float32' if small else 'float64'
    mask=0x7f800000 if small else 0x7ff0000000000000
    edge=[0,1,2,3,(1<<(23 if small else 52))-1,1<<(23 if small else 52),mask-1]
    edge += [v|(1<<(width-1)) for v in edge]
    for v in edge:
        for axis in range(3):
            raw=[0,0,0];raw[axis]=v;cases.append((small,raw))
    for i in range(200):
        raw=[r.getrandbits(width) for _ in range(3)]
        if any(v&mask==mask for v in raw):continue
        cases.append((small,raw))
    for i in range(150):
        vals=[r.randint(-65536,65536)/8192 for _ in range(3)]
        raw=[exact.round_bits(Fraction(v),dtype) for v in vals]
        cases.append((small,raw))
lines=[];expected=[]
for small,raw in cases:
    coords=[Fraction(exact.from_bits(v,'float32' if small else 'float64')) for v in raw]
    result=rng.perlin2002_fraction(coords)
    for output_small in [False,True]:
        lines.append(f'{int(small)} {int(output_small)} '+' '.join(f'{v:x}' for v in raw))
        expected.append(exact.round_bits(result,'float32' if output_small else 'float64'))
start=time.perf_counter()
p=subprocess.run([args.runner,'--tiled-stdin' if args.tiled else '--stdin'],input='\n'.join(lines)+'\n',text=True,capture_output=True,check=True)
actual=[int(v,16) for v in p.stdout.split()]
assert len(actual)==len(expected)
for i,(a,b) in enumerate(zip(actual,expected)):
    assert a==b,(i,lines[i],hex(a),hex(b))
print(json.dumps(dict(runner=args.runner,interface="tiled" if args.tiled else "stdin",cases=len(actual),exact_bit_matches=len(actual),runtime_seconds=time.perf_counter()-start,oracle='independent Python Fraction weighted corner polynomial plus exact IEEE rounding')))
