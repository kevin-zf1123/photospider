"""Determinism, FP environment, nonfinite/domain/budget and ROI checks."""
import os
import subprocess
from pathlib import Path
from array import array
import math
import struct
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('backend', nargs='?', default='cpu',
                    choices=('cpu', 'cpu_tiled', 'gpu', 'vulkan'))
parser.add_argument('--runner', type=Path)
parser.add_argument('--plugin', type=Path)
args = parser.parse_args()
build=args.build;out=args.output;out.mkdir(parents=True,exist_ok=True)
base=[str(args.runner or build/'pixeloe_workflow'),
      str(args.plugin or build/'libphotospider_pixeloe.so'),'128','128']
backend=args.backend
base+=['backend='+backend]

def run(args,simd=1,ok=True):
    result=subprocess.run(base+args,env={**os.environ,'PIXELOE_CPU_SIMD':str(simd)},capture_output=True,text=True)
    if (result.returncode==0)!=ok:raise AssertionError(result.stdout+result.stderr)
    return result
if backend in ('gpu', 'vulkan'):
    opposite = 'metal' if backend == 'vulkan' else 'vulkan'
    failed = run(['operation=pixeloe.pixelize_' + opposite + '_native_fp32'], ok=False)
    assert 'error_code=8 ' in failed.stderr and 'live_payload=0 ' in failed.stderr, failed.stderr
files=[out/'scalar.f32',out/'simd.f32',out/'round.f32']
for f,simd,extra in [(files[0],0,[]),(files[1],1,[]),(files[2],1,['rounding=down'])]:
    run(['output='+str(f)]+extra,simd)
assert files[0].read_bytes()==files[1].read_bytes()==files[2].read_bytes()
for workers in [2,4]:
    for simd in [0,1]:
        f=out/f'workers{workers}-simd{simd}.f32'
        run(['workers='+str(workers),'output='+str(f)],simd)
        assert f.read_bytes()==files[0].read_bytes()

run(['empty=true'])
run(['roi=true', 'output='+str(out/'roi.f32')])
assert (out/'roi.f32').read_bytes() == files[0].read_bytes()
for value,name in [(math.nan,'nan'),(math.inf,'infinity'),(-.01,'negative'),(1.01,'above_one')]:
    x=array('f',[.5])*(128*128*3);x[(64*128+64)*3+1]=value
    (out/'invalid.f32').write_bytes(x.tobytes())
    run(['input='+str(out/'invalid.f32')],ok=False)
run(['pixel_size=1'],ok=False);run(['mode=unknown'],ok=False);run(['budget=1024'],ok=False)
for args, code in [(['cancel_after_us=0'], 6),
                   (['work_budget=1'], 5)]:
    failed=run(args,ok=False)
    assert ('error_code='+str(code)+' ') in failed.stderr and 'live_payload=0 ' in failed.stderr, failed.stderr
# Exact source-bit preservation for nearest, including signed zero/subnormals.
x=array('f',[0.])*(128*128*3)
for y in range(0,128,2):
    for col in range(0,128,2):
        x[(y*128+col)*3]=-0.
        x[(y*128+col)*3+1]=struct.unpack('=f',struct.pack('=I',1))[0]
(out/'tiny.f32').write_bytes(x.tobytes())
run(['input='+str(out/'tiny.f32'),'output='+str(out/'tiny-output.f32'),'pixel_size=2','thickness=0','do_color_match=false','mode=nearest'])
expected=array('f')
for y in range(128):
    for col in range(128):
        start=((y//2*2)*128+col//2*2)*3
        expected.extend(x[start:start+3])
assert (out/'tiny-output.f32').read_bytes()==expected.tobytes()
# Constant black's Lab variance is zero: defined failure, no hidden NaN clamp.
(out/'black.f32').write_bytes(bytes(128*128*12));run(['input='+str(out/'black.f32')],ok=False)
for tag in ['rgb','group']:
    run(['metadata='+tag,'output='+str(out/'tagged.f32')])
    assert (out/'tagged.f32').read_bytes()==files[0].read_bytes()
for tag in ['bgr','d50','linear','scene','unit']:
    run(['metadata='+tag],ok=False)
# Each frame/layer is computed independently under one conservative Whole observation.
source=array('f')
expected=array('f')
for batch in range(4):
    for y in range(128):
        for col in range(128):
            for c in range(3):
                source.append((batch+1)/8+c/32+(col%2)/64)
                expected.append((batch+1)/8+c/32)
(out/'batch.f32').write_bytes(source.tobytes())
run(['frames=2','layers=2','input='+str(out/'batch.f32'),
     'output='+str(out/'batch-output.f32'),'pixel_size=2','thickness=0',
     'do_color_match=false','mode=nearest'])
assert (out/'batch-output.f32').read_bytes()==expected.tobytes()
print('PASS '+backend+': repeat determinism across 1/2/4 workers and SIMD settings, caller FP environment, Whole demand expansion, invalid input/params/budget, cancellation/work rejection cleanup, signed zero/subnormal copy, zero-variance failure, declared color admission')
