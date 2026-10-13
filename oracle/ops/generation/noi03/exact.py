from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac


def _boundary_index(i,n,mode):
    if 0<=i<n:return i
    if mode=='zero':return None
    if mode=='clamp':return max(0,min(n-1,i))
    if mode=='wrap':return i%n
    if mode=='reflect101':
        if n==1:return 0
        j=i%(2*(n-1));return j if j<n else 2*(n-1)-j
    raise ValueError('boundary')


def correlate_kernel(source,kernel,boundary='zero',normalization='none'):
    # Scalar HW fixture; channels are independent copies of this formula.
    h,w=len(source),len(source[0]);kh,kw=len(kernel),len(kernel[0])
    if kh%2!=1 or kw%2!=1:raise ValueError('odd kernel extents')
    if any(len(row)!=w for row in source) or any(len(row)!=kw for row in kernel):raise ValueError('ragged input')
    k=[list(map(frac,row)) for row in kernel]
    den=F(1) if normalization=='none' else sum(map(sum,k),F(0))
    if normalization not in ('none','sum') or not den:raise ValueError('normalization; l2 is separate measured fixture')
    result=[]
    for y in range(h):
        row=[]
        for x in range(w):
            total=F(0)
            for j in range(kh):
                yy=_boundary_index(y+j-kh//2,h,boundary)
                for i in range(kw):
                    xx=_boundary_index(x+i-kw//2,w,boundary)
                    if yy is not None and xx is not None:total+=k[j][i]*frac(source[yy][xx])
            row.append(total/den)
        result.append(row)
    return result
