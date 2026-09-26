"""Raw rank resource validation/addressing oracle, NOT blue-noise qualification.

Synthetic fixtures exercise permutation, hash, periodicity and threshold counts.
No third-party blue-noise/STBN production assets are included or approved.
"""
from __future__ import annotations
import hashlib
import struct
from fractions import Fraction as F
from exact import round_bits, from_bits, float_bits, round_fraction


def _shape_and_flat(data):
    if not isinstance(data,(list,tuple)) or not data:raise ValueError('positive resource extents')
    if all(isinstance(v,int) and not isinstance(v,bool) for v in data):return (len(data),),list(data)
    sub=[_shape_and_flat(v) for v in data]
    if any(s!=sub[0][0] for s,_ in sub):raise ValueError('ragged resource')
    return (len(data),)+sub[0][0],[v for _,values in sub for v in values]


def validate_rank(rank,manifest=None):
    shape,flat=_shape_and_flat(rank);N=len(flat)
    if len(shape) not in (2,3) or N>(1<<40):raise ValueError('rank resource shape')
    if any(v<0 or v>=N for v in flat) or len(set(flat))!=N:raise ValueError('rank must be a complete permutation')
    raw=b''.join(struct.pack('<q',v) for v in flat);digest=hashlib.sha256(raw).hexdigest()
    if manifest is not None:
        if list(shape)!=manifest.get('shape') or digest!=manifest.get('sha256') or manifest.get('encoding')!='little_endian_int64':raise ValueError('resource descriptor/hash mismatch')
    return {'shape':list(shape),'count':N,'sha256':digest,'encoding':'little_endian_int64',
            'qualification':'raw_rank_fixture_only; no blue/STBN certification'}


def _rank_value(r,N,dtype):
    b=round_bits(F(2*r+1,2*N),dtype)
    if b==float_bits(1.0,dtype):b-=1
    if b==0:b=1
    return from_bits(b,dtype)


def rank_lookup(rank,x,y,offset=(0,0),dtype='float64',manifest=None):
    info=validate_rank(rank,manifest)
    if len(info['shape'])!=2:raise ValueError('2D rank required')
    h,w=info['shape'];r=rank[(y+offset[1])%h][(x+offset[0])%w]
    return _rank_value(r,info['count'],dtype)


def threshold_points(rank,height,width,threshold_count,origin=(0,0),offset=(0,0),max_count=1048576):
    info=validate_rank(rank)
    if len(info['shape'])!=2 or height<1 or width<1 or not 0<=threshold_count<=info['count']:raise ValueError('threshold canvas')
    th,tw=info['shape'];out=[]
    for y in range(height):
        for x in range(width):
            if rank[(origin[1]+y+offset[1])%th][(origin[0]+x+offset[0])%tw]<threshold_count:
                if len(out)>=max_count:raise OverflowError('CapacityLimit')
                out.append((round_fraction(F(2*(origin[0]+x)+1,2)),round_fraction(F(2*(origin[1]+y)+1,2))))
    return out


def stbn_lookup(rank,x,y,frame,offset=(0,0,0),dtype='float64',manifest=None):
    info=validate_rank(rank,manifest)
    if len(info['shape'])!=3:raise ValueError('3D rank required')
    T,h,w=info['shape'];r=rank[(frame+offset[2])%T][(y+offset[1])%h][(x+offset[0])%w]
    return _rank_value(r,info['count'],dtype)


def spectral_diagnostics(rank):
    """Optional measured PSD summaries; has NO pass/fail quality threshold."""
    import numpy as np
    info=validate_rank(rank);a=np.asarray(rank,dtype=np.float64)/info['count'];a-=a.mean()
    layers=a[None,...] if a.ndim==2 else a
    spatial=np.abs(np.fft.fft2(layers,axes=(-2,-1)))**2
    spatial[...,0,0]=0
    h,w=a.shape[-2:];fy=np.fft.fftfreq(h)[:,None];fx=np.fft.fftfreq(w)[None,:]
    rr=np.sqrt(fx*fx+fy*fy);low=(rr>0)&(rr<=.125)
    report={'quality':'Measured_no_qualification','spatial_non_dc_power':float(spatial.sum()),
            'spatial_low_frequency_bin_count':int(low.sum()),
            'spatial_low_frequency_power':float(spatial[...,low].sum())}
    if a.ndim==3:
        temporal=np.abs(np.fft.fft(a,axis=0))**2;temporal[0]=0
        report['temporal_non_dc_power']=float(temporal.sum())
        report['temporal_period_frames']=int(a.shape[0])
    return report
