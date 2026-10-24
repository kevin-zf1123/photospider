"""Exact sparse L=1 Local Laplacian reference for large-image checkpoints.

This evaluates the FIL-15 expressions with Fraction, directly from an input
callback. It does not call the C++ implementation or its optimization helpers.
The original small exhaustive FIL-17 reference remains the multi-level oracle.
"""
from functools import lru_cache
from fractions import Fraction as Q
from oracle.ops.filter.common.core import frac, rn
from oracle.ops.filter.common.bounds import certify


def reflect(i, n):
    i %= 2*n
    return i if i < n else 2*n-1-i


class OneLevel:
    def __init__(self, h, w, source, *, sigma, alpha, beta, dtype):
        self.h,self.w,self.source=h,w,source
        self.sigma,self.alpha,self.beta=map(frac,(sigma,alpha,beta))
        self.dtype=dtype

    def remap(self, value, guide):
        v,g=frac(value),frac(guide)
        d=v-g;z=abs(d);sign=1 if d>=0 else -1
        if not z:return rn(g,self.dtype)
        if z>self.sigma:
            return rn(g+sign*(self.sigma+self.beta*(z-self.sigma)),self.dtype)
        if self.alpha.denominator==1:
            return rn(g+sign*self.sigma*(z/self.sigma)**self.alpha.numerator,self.dtype)
        return certify(lambda c:c.q(g)+sign*self.sigma*c.q(z/self.sigma).pow_rational(self.alpha),self.dtype)

    def reduce(self, source, y, x):
        kernel=(1,4,6,4,1)
        total=sum((frac(source(reflect(2*y+j,self.h),reflect(2*x+i,self.w)))*
                   kernel[j+2]*kernel[i+2]
                   for j in range(-2,3) for i in range(-2,3)),Q(0))
        return rn(total/256,self.dtype)

    def expand(self, source, y, x):
        kernel=(1,4,6,4,1);h=(self.h+1)//2;w=(self.w+1)//2
        total=sum((frac(source(reflect((y-j)//2,h),reflect((x-i)//2,w)))*
                   kernel[j+2]*kernel[i+2]
                   for j in range(-2,3) if (y-j)%2==0
                   for i in range(-2,3) if (x-i)%2==0),Q(0))
        return rn(total/64,self.dtype)

    @lru_cache(None)
    def original_coarse(self,y,x):
        return self.reduce(self.source,y,x)

    def output(self,y,x):
        if self.alpha==1 and self.beta==1:return self.source(y,x)
        g=self.source(y,x)
        @lru_cache(None)
        def mapped(j,i):return self.remap(self.source(j,i),g)
        @lru_cache(None)
        def coarse(j,i):return self.reduce(mapped,j,i)
        detail=rn(frac(mapped(y,x))-frac(self.expand(coarse,y,x)),self.dtype)
        return rn(frac(detail)+frac(self.expand(self.original_coarse,y,x)),self.dtype)
