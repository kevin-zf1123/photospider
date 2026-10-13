from __future__ import annotations
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil15.multiscale import pyramid
import copy

def local_laplacian(image,*,levels=1,edge_threshold=1,detail_exponent=1,edge_slope=1,dtype='float64'):
    h,w=shape(image);s,a,b=map(frac,(edge_threshold,detail_exponent,edge_slope))
    if s<=0 or a<=0 or b<0:raise DomainError('local Laplacian parameters')
    if a==1 and b==1:return copy.deepcopy(image)
    if h*w>36:raise DomainError('local Laplacian oracle cap 36 pixels')
    gauss=pyramid(image,levels,mode='gaussian',dtype=dtype);out=pyramid(image,levels,mode='laplacian',dtype=dtype)
    gs={v['level']:v['values'] for v in gauss['bands']}
    for band in out['bands']:
        if band['role']=='base':continue
        l=band['level'];hh,ww=shape(band['values'])
        for y in range(hh):
            for x in range(ww):
                g=frac(gs[l][y][x]);remapped=zeros(h,w)
                for j in range(h):
                    for i in range(w):
                        d=frac(image[j][i])-g;z=abs(d);sign=1 if d>=0 else -1
                        if not z:value=rn(g,dtype)
                        elif z>s:value=rn(g+sign*(s+b*(z-s)),dtype)
                        elif a.denominator==1:value=rn(g+sign*s*(z/s)**a.numerator,dtype)
                        else:value=certify(lambda c:c.q(g)+sign*s*c.q(z/s).pow_rational(a),dtype)
                        remapped[j][i]=value
                local=pyramid(remapped,levels,mode='laplacian',dtype=dtype)
                source=next(v for v in local['bands'] if v['level']==l and v['role']=='detail')
                band['values'][y][x]=source['values'][y][x]
    return pyramid(mode='reconstruct',bands=out,dtype=dtype)
