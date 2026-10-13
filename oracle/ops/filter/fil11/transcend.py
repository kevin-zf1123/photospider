from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import zeros

def log_kernel(sigma,radius,*,scale_normalized=True):
    s=frac(sigma)
    if s<=0 or radius<1:raise DomainError('LoG parameters')
    a=zeros(2*radius+1,2*radius+1);cache={}
    for y in range(-radius,radius+1):
        for x in range(-radius,radius+1):
            r2=x*x+y*y
            if r2 not in cache:
                q=(r2-2*s*s)/(2*s**6)*(s*s if scale_normalized else 1)
                cache[r2]=0.0 if not q else certify(lambda c:c.q(q)*c.q(-Q(r2)/(2*s*s)).exp()/c.pi())
            a[y+radius][x+radius]=cache[r2]
    return a,rn(sum((frac(v) for row in a for v in row),Q()))
