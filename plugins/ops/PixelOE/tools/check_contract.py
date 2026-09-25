"""Determinism, FP environment, nonfinite/domain/budget and ROI checks."""
import os
import subprocess
import sys
from pathlib import Path
import numpy as np
build=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
base=[str(build/'pixeloe_workflow'),str(build/'libphotospider_pixeloe.so'),'128','128']
def run(args,simd=1,ok=True):
    result=subprocess.run(base+args,env={**os.environ,'PIXELOE_CPU_SIMD':str(simd)},capture_output=True,text=True)
    if (result.returncode==0)!=ok:raise AssertionError(result.stdout+result.stderr)
    return result
files=[out/'scalar.f32',out/'simd.f32',out/'round.f32']
for f,simd,extra in [(files[0],0,[]),(files[1],1,[]),(files[2],1,['rounding=down'])]:
    run(['output='+str(f)]+extra,simd)
assert files[0].read_bytes()==files[1].read_bytes()==files[2].read_bytes()
run(['roi=true'],ok=False)  # Current planar Whole contract requires a whole output request.
for value,name in [(np.nan,'nan'),(np.inf,'infinity'),(-.01,'negative'),(1.01,'above_one')]:
    x=np.full((128,128,3),.5,np.float32);x[64,64,1]=value;x.tofile(out/'invalid.f32')
    run(['input='+str(out/'invalid.f32')],ok=False)
run(['pixel_size=1'],ok=False);run(['mode=unknown'],ok=False);run(['budget=1024'],ok=False)
# Exact source-bit preservation for nearest, including signed zero/subnormals.
x=np.zeros((128,128,3),np.float32);x[::2,::2,0]=-0.;x[::2,::2,1]=np.nextafter(np.float32(0),np.float32(1));x.tofile(out/'tiny.f32')
run(['input='+str(out/'tiny.f32'),'output='+str(out/'tiny-output.f32'),'pixel_size=2','thickness=0','do_color_match=false','mode=nearest'])
y=np.repeat(np.repeat(x[::2,::2],2,0),2,1)
assert (out/'tiny-output.f32').read_bytes()==y.tobytes()
# Constant black's Lab variance is zero: defined failure, no hidden NaN clamp.
x.fill(0);x.tofile(out/'black.f32');run(['input='+str(out/'black.f32')],ok=False)
for tag in ['rgb','group']:
    run(['metadata='+tag,'output='+str(out/'tagged.f32')])
    assert (out/'tagged.f32').read_bytes()==files[0].read_bytes()
for tag in ['bgr','d50','linear','scene','unit']:
    run(['metadata='+tag],ok=False)
print('PASS: scalar/SIMD bitwise, caller FP environment, partial Whole rejection, invalid input/params/budget, signed zero/subnormal copy, zero-variance failure, declared color admission')
