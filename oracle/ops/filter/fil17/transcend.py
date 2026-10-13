from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros

def local_contrast(image,base,*,amount=0,detail_scale=1,dtype='float64'):
    h,w=same_shape(image,base);amount,scale=frac(amount),frac(detail_scale)
    if scale<=0:raise DomainError('detail scale')
    if not amount:return [list(row) for row in image]
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a,b=frac(image[y][x]),frac(base[y][x]);d=a-b
            out[y][x]=rn(a,dtype) if not d else certify(lambda c:c.q(a)+amount*scale*c.q(d/scale).tanh(),dtype)
    return out
