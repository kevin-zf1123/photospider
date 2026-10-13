from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil09.transcend import _atan_interval

def eigen2d(a,b,c,*,dtype='float64'):
    h,w=same_shape(a,b,c);large=zeros(h,w);small=zeros(h,w);angle=zeros(h,w);valid=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            aa,bb,cc=map(frac,(a[y][x],b[y][x],c[y][x]));d=(aa-cc)**2+4*bb*bb
            if not bb:
                large[y][x]=rn(max(aa,cc),dtype);small[y][x]=rn(min(aa,cc),dtype)
            else:
                large[y][x]=certify(lambda ctx:(aa+cc+ctx.q(d).sqrt())/2,dtype)
                # A rational zero determinant gives an exactly zero eigenvalue.
                if aa*cc==bb*bb and aa+cc>=0:small[y][x]=0.0
                else:small[y][x]=certify(lambda ctx:(aa+cc-ctx.q(d).sqrt())/2,dtype)
            if aa!=cc or bb:
                valid[y][x]=1
                angle[y][x]=certify(lambda ctx:_atan_interval(ctx,2*bb,aa-cc)/2+(ctx.pi() if bb<0 else 0),dtype)
    return large,small,angle,valid
