from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil02.spatial import separable
from oracle.ops.filter.frq10.multiscale import _max_decimated_levels
import copy

BINOMIAL=[Q(1,16),Q(4,16),Q(6,16),Q(4,16),Q(1,16)]

def reduce_level(image,dtype='float64'):
    blurred=separable(image,BINOMIAL,BINOMIAL,(2,2),boundary='reflect_half',dtype=dtype)
    h,w=shape(image)
    return [[blurred[y][x] for x in range(0,w,2)] for y in range(0,h,2)]

def expand_level(coarse,target_shape,dtype='float64'):
    h,w=target_shape
    if shape(coarse)!=((h+1)//2,(w+1)//2):raise DomainError('expand target shape')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            total=Q()
            for dy in range(-2,3):
                for dx in range(-2,3):
                    if (y-dy)%2 or (x-dx)%2:continue
                    total+=4*BINOMIAL[dy+2]*BINOMIAL[dx+2]*sample(coarse,(y-dy)//2,(x-dx)//2,'reflect_half')
            out[y][x]=rn(total,dtype)
    return out

def pyramid(image=None,levels=0,*,mode='laplacian',bands=None,dtype='float64'):
    if mode=='reconstruct':
        if bands is None or bands.get('profile')!='pyramid_binomial5_v1' or bands.get('kind')!='laplacian':raise DomainError('pyramid schema/profile')
        levels=bands['levels'];shapes=[tuple(s) for s in bands['shapes']]
        if len(shapes)!=levels+1 or tuple(bands['original_shape'])!=shapes[0]:raise DomainError('pyramid shape history')
        by={(b['level'],b['role']):b['values'] for b in bands['bands']}
        expected={(l,'detail') for l in range(levels)}|{(levels,'base')}
        if set(by)!=expected or len(by)!=len(bands['bands']):raise DomainError('pyramid role set')
        if any(shapes[l+1]!=((shapes[l][0]+1)//2,(shapes[l][1]+1)//2) for l in range(levels)):raise DomainError('pyramid shape progression')
        current=copy.deepcopy(by[(levels,'base')])
        if shape(current)!=shapes[-1]:raise DomainError('base shape')
        for l in range(levels-1,-1,-1):
            detail=by[(l,'detail')]
            if shape(detail)!=shapes[l]:raise DomainError('detail shape')
            ex=expand_level(current,shapes[l],dtype);h,w=shapes[l]
            current=[[rn(frac(detail[y][x])+frac(ex[y][x]),dtype) for x in range(w)] for y in range(h)]
        return current
    h,w=shape(image)
    if levels<0 or levels>_max_decimated_levels(h,w):raise DomainError('pyramid levels')
    if mode not in ('gaussian','laplacian'):raise DomainError('pyramid mode')
    gs=[copy.deepcopy(image)]
    for _ in range(levels):gs.append(reduce_level(gs[-1],dtype))
    result={'schema':'FilterBands/v1','profile':'pyramid_binomial5_v1','kind':mode,'levels':levels,'dtype':dtype,'original_shape':[h,w],'shapes':[list(shape(a)) for a in gs],'bands':[]}
    if mode=='gaussian':
        result['bands']=[{'level':l,'role':'gaussian','values':v} for l,v in enumerate(gs)]
    else:
        for l in range(levels):
            hh,ww=shape(gs[l]);ex=expand_level(gs[l+1],(hh,ww),dtype)
            values=[[rn(frac(gs[l][y][x])-frac(ex[y][x]),dtype) for x in range(ww)] for y in range(hh)]
            result['bands'].append({'level':l,'role':'detail','values':values})
        result['bands'].append({'level':levels,'role':'base','values':gs[-1]})
    return result
