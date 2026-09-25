"""Freeze upstream coefficient construction as binary32 C++ constants."""
import sys
from pathlib import Path
import numpy as np
import torch

tables = {}
for radius in [2,4,8,16,32]:
    x=torch.arange(-radius,radius+1,dtype=torch.float16)
    k=torch.exp(-x.pow(2)/(2*radius*radius))
    k2=k[None,:]*k[:,None]
    exact=(k2/k2.sum()).float().numpy()
    tables[f'exact{radius}']=exact.reshape(-1)
    u,s,v=np.linalg.svd(exact.astype(np.float64))
    # Keep complete row/column bases; runtime selects a requested rank.
    tables[f'lr{radius}']=np.concatenate([v.reshape(-1),(u*s).T.reshape(-1)]).astype(np.float32)
    x=np.arange(-radius,radius+1,dtype=np.float64)
    k=np.exp(-x*x/(2*radius*radius))
    tables[f'sep{radius}']=(k/k.sum()).astype(np.float32)
x=torch.arange(-1,2).float(); k=torch.exp(-x*x/2); k=k/k.sum()
tables['unsharp']=(k[:,None]*k[None,:]).numpy().reshape(-1)
tables['laplacian']=np.array([0,-1,0,-1,4,-1,0,-1,0],np.float32)
for it,r in enumerate([1,1.5,2,2.5,3,3.5],1):
    radius=int(r); size=2*radius+1
    kernel=np.zeros((size,size))
    for y in range(size):
        for x in range(size):
            pts=np.array([[y,x]]*8)+np.array([[-.5,-.5],[-.5,.5],[.5,-.5],[.5,.5],[0,.5],[0,-.5],[.5,0],[-.5,0]])
            d=np.linalg.norm(pts-radius,axis=1); lo,hi=d.min(),d.max()
            kernel[y,x]=1 if hi<=r else ((r-lo)/(hi-lo) if lo<=r else 0)
    if it in (3,5): kernel=kernel[1:-1,1:-1]
    tables[f'se{it}']=kernel.astype(np.float32).reshape(-1)
b=np.array([[0,2],[3,1]])
while len(b)<8: b=np.block([[4*b,4*b+2],[4*b+3,4*b+1]])
tables['bayer']=(b/64).astype(np.float32).reshape(-1)
for n in range(2,257):
    if n<8: interp=torch.linspace(0,1,n)[:,None].expand(-1,3)
    else:
        base=n//4; cent=n-base*3
        a=(torch.linspace(0,1,base+1)[1:,None,None]*torch.eye(3)).reshape(-1,3)
        c=torch.linspace(0,1,cent)[:,None].expand(-1,3)
        interp=torch.cat([a,c])
    tables[f'interp{n}']=interp.contiguous().numpy().reshape(-1)
lines=['#include "runtime.hpp"','namespace px {','const std::vector<float>& table(const std::string& key) {','static const std::map<std::string,std::vector<float>> tables = {']
for name,data in tables.items():
    values=','.join(float(v).hex()+'f' for v in data)
    lines.append('{"'+name+'", {'+values+'}},')
lines+=['}; return tables.at(key);','}','}']
Path(sys.argv[1]).write_text('\n'.join(lines)+'\n')
