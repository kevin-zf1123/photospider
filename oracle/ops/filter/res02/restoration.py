from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import exp_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import coordinate
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil07.spatial import clipped_box
import copy

def nlm(image,guide,*,patch_radius=1,search_radius=1,h_parameter=1,metric_scale=None,noise_sigma=None,dtype='float64'):
    h,w=same_shape(image,guide);hp=frac(h_parameter)
    if min(patch_radius,search_radius)<0 or hp<=0:raise DomainError('NLM parameters')
    sigma=Q() if noise_sigma is None else frac(noise_sigma)
    if sigma<0:raise DomainError('sigma')
    if search_radius==0:return copy.deepcopy(image)
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if not 1<=gdim<=4 or len(scales)!=gdim or min(scales)<=0:raise DomainError('guide metric')
    def g(y,x):
        yy,xx=coordinate(y,x,h,w,'reflect_half')
        return [frac(guide[yy][xx][c] if vector else guide[yy][xx])*scales[c] for c in range(gdim)]
    out=zeros(h,w);npatch=(2*patch_radius+1)**2*gdim
    for y in range(h):
        for x in range(w):
            num=den=Q()
            for j,i in clipped_box(y,x,h,w,search_radius,search_radius):
                if (j,i)==(y,x):weight=Q(1)
                else:
                    distance=Q()
                    for dy in range(-patch_radius,patch_radius+1):
                        for dx in range(-patch_radius,patch_radius+1):
                            a,b=g(y+dy,x+dx),g(j+dy,i+dx)
                            distance+=sum(((v-u)**2 for v,u in zip(a,b)),Q())
                    distance/=npatch
                    if noise_sigma is not None:distance=max(distance-2*sigma*sigma,0)
                    weight=frac(exp_q(-distance/(hp*hp)))
                if weight:num+=weight*frac(image[j][i]);den+=weight
            out[y][x]=rn(num/den,dtype)
    return out
