"""Independent Philox integer oracle and exact finite noise models.

Philox4x64-10 is checked against external Random123 KATs (see SOURCES.md).
Counter packing, seed-to-key injection and Float32/Float64 half-open and open52 extraction match production v1.
Member word selections and address allocations follow the shared v1 contract.
Python domain guards reject unsupported oracle probes only; the production
contract assumes valid payloads without runtime semantic validation.
No upstream implementation is bundled. Bridson's transcendental candidates below
are explicitly MEASURED, not correctly-rounded strict golden sequences.
"""

from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
import math

MASK=(1<<64)-1
MASK32=(1<<32)-1
RNG_PROTOCOL="philox4x64-v1"
DOMAINS={'uniform':1,'gaussian':2,'jitter':3,'rejection':4,'bridson':5,
         'gradient':6,'cellular':7,'poisson':8,'speckle':9,'grain':10,'resource':11}


def _integer(v,lo,hi,name):
    if not isinstance(v,int) or isinstance(v,bool) or not lo<=v<=hi:
        raise ValueError(f'{name} outside [{lo},{hi}]')
    return v


def _inside_map(u,lo,hi):
    lo,hi,u=frac(lo),frac(hi),frac(u)
    if not lo<hi or not 0<=u<1:raise ValueError('half-open interval')
    x=round_fraction(lo+(hi-lo)*u)
    if F(x)>=hi:
        x=math.nextafter(round_fraction(hi),-math.inf)
    if not lo<=F(x)<hi:raise ValueError('no representable interior value')
    return x


def _bounds(bounds):
    if len(bounds)!=4:raise ValueError('bounds width')
    x0,y0,x1,y1=map(frac,bounds)
    if x0>=x1 or y0>=y1:raise ValueError('positive area bounds')
    return x0,y0,x1,y1


def distance2(a,b):return sum((frac(x)-frac(y))**2 for x,y in zip(a,b))
