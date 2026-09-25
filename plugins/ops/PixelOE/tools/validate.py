"""Independent upstream Torch oracle, plus public workflow option coverage."""
import argparse
import importlib
import json
import subprocess
import sys
from pathlib import Path
import numpy as np
import torch

p=argparse.ArgumentParser()
p.add_argument('--upstream',type=Path,required=True)
p.add_argument('--runner',type=Path,required=True)
p.add_argument('--plugin',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(a.upstream/'src'))
from pixeloe.torch.pixelize import pixelize
from pixeloe.torch.outline import outline_expansion
import torch.nn.functional as F
pixel_module=importlib.import_module('pixeloe.torch.pixelize')
color_module=importlib.import_module('pixeloe.torch.color')
outline_module=importlib.import_module('pixeloe.torch.outline')
original_match=pixel_module.match_color
original_stat=outline_module.local_stat

def independent_match(source,target,opts):
    sl=color_module.rgb_to_lab(source);tl=color_module.rgb_to_lab(target)
    corrected=color_module.lab_to_rgb((sl-sl.mean())/sl.std()*tl.std()+tl.mean())
    diff=target-corrected
    for r in [2,4,8,16,32]:
        mode='reflect' if min(diff.shape[-2:])>r else 'replicate'
        if opts.get('colorfix_blur')=='separable':
            xx=np.arange(-r,r+1,dtype=np.float64);kk=np.exp(-xx*xx/(2*r*r));kk=(kk/kk.sum()).astype(np.float32)
            rows=[kk];cols=[kk]
        else:
            full=color_module.gaussian_kernel(r,torch.device('cpu')).float().numpy().astype(np.float64)
            u,v,vt=np.linalg.svd(full);rank=min(opts.get('blur_rank',1),2*r+1)
            rows=vt[:rank].astype(np.float32);cols=(u[:,:rank]*v[:rank]).T.astype(np.float32)
        out=torch.zeros_like(diff)
        for row,col in zip(rows,cols):
            temp=F.conv2d(F.pad(diff,(r,r,0,0),mode=mode),torch.from_numpy(row.copy()).reshape(1,1,1,-1).repeat(3,1,1,1),groups=3)
            out+=F.conv2d(F.pad(temp,(0,0,r,r),mode=mode),torch.from_numpy(col.copy()).reshape(1,1,-1,1).repeat(3,1,1,1),groups=3)
        diff=out
    return corrected+diff

def independent_sliding(tensor,kernel,stride,stat,padding):
    lo=kernel//2;hi=kernel-1-lo
    padded=F.pad(tensor,(lo,hi,lo,hi),mode='replicate' if padding=='replicate' else 'constant')
    patches=F.unfold(padded,kernel_size=kernel)
    values={'median':lambda:patches.median(dim=1).values,'min':lambda:patches.amin(dim=1),'max':lambda:patches.amax(dim=1)}[stat]()
    return values.reshape(tensor.shape)


torch.set_num_threads(1)
rng=np.random.default_rng(7921)
x=rng.uniform(.02,.98,(25,31,3)).astype(np.float32)
infile=a.out/'input.f32';x.tofile(infile)
t=torch.from_numpy(x).permute(2,0,1)[None]
records=[]
def run(name,opts,oracle=True,tol=1e-5):
    outfile=a.out/(name+'.f32')
    cli=[str(a.runner),str(a.plugin),'31','25','input='+str(infile),'output='+str(outfile)]
    cli += [f'{k}={str(v).lower() if isinstance(v,bool) else v}' for k,v in opts.items()]
    r=subprocess.run(cli,text=True,capture_output=True)
    if r.returncode: raise RuntimeError(name+': '+r.stderr)
    actual=np.fromfile(outfile,np.float32)
    entry={'name':name,'workflow':r.stdout.strip(),'finite':bool(np.isfinite(actual).all())}
    if oracle:
        args={k:v for k,v in opts.items() if k not in ('blur_impl','colorfix_blur','blur_rank','local_stats','stat_padding')}
        if opts.get('local_stats')=='sliding':
            outline_module.local_stat=lambda tensor,kernel,stride,stat:independent_sliding(tensor,kernel,stride,stat,opts.get('stat_padding','zero'))
        if opts.get('colorfix_blur')=='separable' or opts.get('blur_impl','lowrank')=='lowrank':
            pixel_module.match_color=lambda source,target:independent_match(source,target,opts)
        expected=pixelize(t,backend='torch',**args).detach().permute(0,2,3,1).numpy().reshape(-1)
        pixel_module.match_color=original_match;outline_module.local_stat=original_stat
        error=float(np.max(np.abs(actual-expected)))
        entry.update(max_abs=error,tolerance=tol,passed=error<=tol)
        if error>tol:
            records.append(entry);(a.out/'validation.json').write_text(json.dumps(records,indent=2));raise AssertionError(entry)
    records.append(entry);print(json.dumps(entry),flush=True)

for mode in ['contrast','k_centroid','nearest','nearest-exact','bilinear','bicubic','area','lanczos']:
    run('mode_'+mode,{'mode':mode,'do_color_match':False},tol=1e-5)
for thick in [0,1,2,4,5,6]:
    run('thickness_'+str(thick),{'thickness':thick,'do_color_match':False})
for mode in ['unsharp','laplacian']:
    run('sharpen_'+mode,{'sharpen_mode':mode,'do_color_match':False})
for mode in ['current','polarity','contrast_ratio','contrast_gated']:
    run('weight_'+mode,{'weight_mapping':mode,'do_color_match':False})
for mode in ['none','per_image']:
    run('normalize_'+mode,{'weight_normalize':mode,'do_color_match':False})
run('small_output',{'no_post_upscale':True,'do_color_match':False})
# Different reduction/convolution orders need a continuous tolerance, never a bitwise claim.
run('default_lowrank',{})
run('color_exact',{'blur_impl':'direct'},tol=1e-5)
for mode in ['kmeans','weighted-kmeans','repeat-kmeans']:
    for dither in ['none','ordered','error_diffusion']:
        run('quant_'+mode+'_'+dither,{'do_quant':True,'quant_mode':mode,'dither_mode':dither,'blur_impl':'direct','num_colors':8},tol=1e-5)
for opts in [dict(local_stats='sliding'),dict(local_stats='sliding',stat_padding='replicate'),dict(colorfix_blur='separable'),dict(blur_impl='sym'),dict(blur_impl='tiled'),dict(blur_rank=2)]:
    run('option_'+str(len(records)),opts)
(a.out/'validation.json').write_text(json.dumps(records,indent=2)+'\n')
