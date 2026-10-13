"""Raw rank resource validation/addressing oracle, NOT blue-noise qualification.

Synthetic fixtures exercise permutation, hash, periodicity and threshold counts.
No third-party blue-noise/STBN production assets are included or approved.
"""

from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import float_bits
from oracle.ops.generation.common.exact import from_bits
from oracle.ops.generation.common.exact import round_bits


def _shape_and_flat(data):
    if not isinstance(data,(list,tuple)) or not data:raise ValueError('positive resource extents')
    if all(isinstance(v,int) and not isinstance(v,bool) for v in data):return (len(data),),list(data)
    sub=[_shape_and_flat(v) for v in data]
    if any(s!=sub[0][0] for s,_ in sub):raise ValueError('ragged resource')
    return (len(data),)+sub[0][0],[v for _,values in sub for v in values]


def _rank_value(r,N,dtype):
    b=round_bits(F(2*r+1,2*N),dtype)
    if b==float_bits(1.0,dtype):b-=1
    if b==0:b=1
    return from_bits(b,dtype)
