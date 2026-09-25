"""Decode before timing; write FP32 RGB fixtures then time native/public paths."""
import argparse
import json
import subprocess
from pathlib import Path
import cv2
import numpy as np
p=argparse.ArgumentParser()
p.add_argument('--assets',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--repeat',type=int,default=3)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
names=['pattern_rgba16_129x131.png','illustration_rgb8_4962x5454.jpeg','stage_rgba8_3504x4958.tiff']
records=[]
for name in names:
    x=cv2.imread(str(a.assets/name),cv2.IMREAD_UNCHANGED)
    if x is None:raise RuntimeError(name)
    original=str(x.dtype);x=x[:,:,:3][:,:,::-1].astype(np.float32)/float(np.iinfo(x.dtype).max)
    h,w=x.shape[:2];raw=a.out/(name+'.f32');x.tofile(raw)
    print(f'prepared {name}: {w}x{h}, {original}, RGB; alpha discarded if present',flush=True)
    del x
    cmd=[str(a.build/'pixeloe_workflow'),str(a.build/'libphotospider_pixeloe.so'),str(w),str(h),'input='+str(raw),'warmup=1',f'repeat={a.repeat}']
    public=subprocess.run(cmd,text=True,capture_output=True,check=True).stdout.strip()
    native=subprocess.run([str(a.build/'pixeloe_benchmark'),str(w),str(h),str(a.repeat),str(raw)],text=True,capture_output=True,check=True).stdout.strip()
    record={'asset':name,'width':w,'height':h,'decoded_dtype':original,'preparation':'OpenCV decode, RGB reorder, discard alpha, normalize to FP32; all outside timing','public':public,'native':native}
    records.append(record);print(json.dumps(record),flush=True)
    (a.out/'results.json').write_text(json.dumps(records,indent=2)+'\n')
