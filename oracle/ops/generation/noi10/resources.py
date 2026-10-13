from __future__ import annotations
from oracle.ops.generation.common.resources import _rank_value
from oracle.ops.generation.noi07.resources import validate_rank


def stbn_lookup(rank,x,y,frame,offset=(0,0,0),dtype='float64',manifest=None):
    info=validate_rank(rank,manifest)
    if len(info['shape'])!=3:raise ValueError('3D rank required')
    T,h,w=info['shape'];r=rank[(frame+offset[2])%T][(y+offset[1])%h][(x+offset[0])%w]
    return _rank_value(r,info['count'],dtype)
