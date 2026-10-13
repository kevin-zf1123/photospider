from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
import bisect


def width_linear(s,length,stops,widths,normalized=True):
    s,L=frac(s),frac(length);xs=list(map(frac,stops));ys=list(map(frac,widths))
    if len(xs)<2 or len(xs)!=len(ys) or any(a>=b for a,b in zip(xs,xs[1:])) or any(y<0 for y in ys):
        raise ValueError('width profile')
    if L<0 or not 0<=s<=L:raise ValueError('arc coordinate')
    if normalized and (xs[0]!=0 or xs[-1]!=1):raise ValueError('normalized stops')
    q=(s/L if L else F(0)) if normalized else s
    if not xs[0]<=q<=xs[-1]:raise ValueError('outside stops')
    i=bisect.bisect_left(xs,q)
    if xs[i]==q:return ys[i]
    t=(q-xs[i-1])/(xs[i]-xs[i-1]);return (1-t)*ys[i-1]+t*ys[i]


def pchip_fixture(xs,ys,q):
    # Independent Fraction implementation of the same published shape-preserving
    # formulas. This helper has no color/descriptor semantics.
    x,y=list(map(frac,xs)),list(map(frac,ys));q=frac(q)
    if len(x)<2 or len(x)!=len(y) or any(a>=b for a,b in zip(x,x[1:])) or not x[0]<=q<=x[-1]:raise ValueError('pchip domain')
    h=[b-a for a,b in zip(x,x[1:])];d=[(b-a)/z for a,b,z in zip(y,y[1:],h)]
    def endpoint(h0,h1,d0,d1):
        z=((2*h0+h1)*d0-h0*d1)/(h0+h1)
        if z*d0<=0:return F(0)
        return 3*d0 if d0*d1<0 and abs(z)>3*abs(d0) else z
    if len(x)==2:m=[d[0],d[0]]
    else:
        m=[endpoint(h[0],h[1],d[0],d[1])]
        for i in range(1,len(x)-1):
            if d[i-1]*d[i]<=0:m.append(F(0))
            else:
                w1,w2=2*h[i]+h[i-1],h[i]+2*h[i-1]
                m.append((w1+w2)/(w1/d[i-1]+w2/d[i]))
        m.append(endpoint(h[-1],h[-2],d[-1],d[-2]))
    j=bisect.bisect_left(x,q)
    if x[j]==q:return y[j]
    j-=1;t=(q-x[j])/h[j]
    return (2*t**3-3*t*t+1)*y[j]+(t**3-2*t*t+t)*h[j]*m[j]+(-2*t**3+3*t*t)*y[j+1]+(t**3-t*t)*h[j]*m[j+1]


def source_width_coordinate(subpath_lengths,subpath,source_s,parameter_domain):
    """Exact source-position map, not a PathSet association/codec implementation."""
    lengths=list(map(frac,subpath_lengths));s=frac(source_s)
    if not lengths or min(lengths)<0 or not isinstance(subpath,int) or not 0<=subpath<len(lengths):
        raise ValueError('source subpath')
    if not 0<=s<=lengths[subpath]:raise ValueError('source arc coordinate')
    if parameter_domain=='subpath_arclength_px':return s
    if parameter_domain=='subpath_normalized_arclength':
        return s/lengths[subpath] if lengths[subpath] else F(0)
    if parameter_domain=='pathset_normalized_arclength':
        total=sum(lengths)
        return (sum(lengths[:subpath])+s)/total if total else F(0)
    raise ValueError('parameter domain')
