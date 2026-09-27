"""MPFR 4.2+ LP64 directed interval arithmetic with bounded refinement.

All expression nodes are enclosed. Matching RN-even endpoint bits certify the
entire expression, unlike merely comparing two high-precision approximations.
This reference does not change MPFR global exponent limits or the host fenv.
It is deliberately small and externally dependent; absence is an explicit gate.
"""
from __future__ import annotations
import ctypes as C
import ctypes.util
import os
import math
from .core import frac, rn, from_bits, bits, Inconclusive, DomainError

class _MP(C.Structure):
    _fields_=[('precision',C.c_long),('sign',C.c_int),('exponent',C.c_long),('limbs',C.POINTER(C.c_ulong))]

class Context:
    RN=0; UP=2; DOWN=3
    def __init__(self, precision=128):
        if C.sizeof(C.c_long)!=8: raise RuntimeError('LP64 MPFR ABI required')
        path=os.getenv('PHOTOSPIDER_ORACLE_MPFR') or ctypes.util.find_library('mpfr')
        if not path: raise RuntimeError('MPFR 4.2+ not found')
        self.lib=C.CDLL(path);self.values=[];self.precision=precision
        L=self.lib;P=C.POINTER(_MP)
        L.mpfr_get_version.restype=C.c_char_p
        self.version=L.mpfr_get_version().decode()
        if tuple(map(int,self.version.split('.')[:2]))<(4,2): raise RuntimeError('MPFR 4.2+ required')
        L.mpfr_init2.argtypes=[P,C.c_long];L.mpfr_clear.argtypes=[P]
        L.mpfr_set_str.argtypes=[P,C.c_char_p,C.c_int,C.c_int]
        L.mpfr_cmp.argtypes=[P,P];L.mpfr_cmp_si.argtypes=[P,C.c_long]
        L.mpfr_nan_p.argtypes=[P];L.mpfr_nan_p.restype=C.c_int
        L.mpfr_get_d.argtypes=[P,C.c_int];L.mpfr_get_d.restype=C.c_double
        L.mpfr_get_flt.argtypes=[P,C.c_int];L.mpfr_get_flt.restype=C.c_float
        L.mpfr_const_pi.argtypes=[P,C.c_int]
        for f in ['exp','sqrt','sin','cos','sinpi','cospi','tanh','neg','log','erf']:
            getattr(L,'mpfr_'+f).argtypes=[P,P,C.c_int]
        for f in ['add','sub','mul','div','pow','atan2']:
            getattr(L,'mpfr_'+f).argtypes=[P,P,P,C.c_int]
    def __enter__(self):return self
    def __exit__(self,*exc):
        for x in reversed(self.values):self.lib.mpfr_clear(C.byref(x))
        self.values.clear()
    def new(self):
        x=_MP();self.lib.mpfr_init2(C.byref(x),self.precision);self.values.append(x);return x
    def integer(self,x,rnd):
        z=self.new();self.lib.mpfr_set_str(C.byref(z),str(x).encode(),10,rnd);return z
    def unary(self,f,x,rnd):
        z=self.new();getattr(self.lib,'mpfr_'+f)(C.byref(z),C.byref(x),rnd)
        if self.lib.mpfr_nan_p(C.byref(z)):raise Inconclusive('NaN interval endpoint; no certificate')
        return z
    def binary(self,f,x,y,rnd):
        z=self.new();getattr(self.lib,'mpfr_'+f)(C.byref(z),C.byref(x),C.byref(y),rnd)
        if self.lib.mpfr_nan_p(C.byref(z)):raise Inconclusive('NaN interval endpoint; no certificate')
        return z
    def cmp(self,a,b):return self.lib.mpfr_cmp(C.byref(a),C.byref(b))
    def cmp0(self,a):return self.lib.mpfr_cmp_si(C.byref(a),0)
    def lo(self,a):
        z=a[0]
        for x in a[1:]:
            if self.cmp(x,z)<0:z=x
        return z
    def hi(self,a):
        z=a[0]
        for x in a[1:]:
            if self.cmp(x,z)>0:z=x
        return z
    def q(self,x):
        q=frac(x)
        n=Interval(self,self.integer(q.numerator,self.DOWN),self.integer(q.numerator,self.UP))
        d=Interval(self,self.integer(q.denominator,self.DOWN),self.integer(q.denominator,self.UP))
        return n/d
    def pi(self):
        a,b=self.new(),self.new()
        self.lib.mpfr_const_pi(C.byref(a),self.DOWN);self.lib.mpfr_const_pi(C.byref(b),self.UP)
        return Interval(self,a,b)
    def rounded(self,a,dtype):
        x=(self.lib.mpfr_get_flt if dtype=='float32' else self.lib.mpfr_get_d)(C.byref(a),self.RN)
        return bits(x,dtype)
    def cospi(self,r):
        r=frac(r)%2
        if r>1:r=2-r
        sign=1
        if r>frac(1)/2:r=1-r;sign=-1
        if r==0:return self.q(sign)
        if r==frac(1)/2:return self.q(0)
        # Exact rational values substantially reduce cancellation uncertainty.
        if r==frac(1)/3:return self.q(frac(sign)/2)
        t=self.q(r)
        v=Interval(self,self.unary('cospi',t.hi,self.DOWN),self.unary('cospi',t.lo,self.UP))
        return v if sign==1 else -v
    def sinpi(self,r):return self.cospi(frac(1)/2-frac(r))

class Interval:
    def __init__(self,ctx,lo,hi):self.ctx=ctx;self.lo=lo;self.hi=hi
    def cast(self,x):return x if isinstance(x,Interval) else self.ctx.q(x)
    def __neg__(self):
        c=self.ctx;return Interval(c,c.unary('neg',self.hi,c.DOWN),c.unary('neg',self.lo,c.UP))
    def __add__(self,b):
        c=self.ctx;b=self.cast(b)
        return Interval(c,c.binary('add',self.lo,b.lo,c.DOWN),c.binary('add',self.hi,b.hi,c.UP))
    __radd__=__add__
    def __sub__(self,b):return self+-self.cast(b)
    def __rsub__(self,b):return self.cast(b)+-self
    def __mul__(self,b):
        c=self.ctx;b=self.cast(b)
        pairs=[(x,y) for x in (self.lo,self.hi) for y in (b.lo,b.hi)]
        return Interval(c,c.lo([c.binary('mul',x,y,c.DOWN) for x,y in pairs]),c.hi([c.binary('mul',x,y,c.UP) for x,y in pairs]))
    __rmul__=__mul__
    def __truediv__(self,b):
        c=self.ctx;b=self.cast(b)
        if c.cmp0(b.lo)<=0<=c.cmp0(b.hi):raise Inconclusive('divisor interval crosses zero')
        pairs=[(x,y) for x in (self.lo,self.hi) for y in (b.lo,b.hi)]
        return Interval(c,c.lo([c.binary('div',x,y,c.DOWN) for x,y in pairs]),c.hi([c.binary('div',x,y,c.UP) for x,y in pairs]))
    def __rtruediv__(self,b):return self.cast(b)/self
    def square(self):
        c=self.ctx
        if c.cmp0(self.lo)>=0:
            return Interval(c,c.binary('mul',self.lo,self.lo,c.DOWN),c.binary('mul',self.hi,self.hi,c.UP))
        if c.cmp0(self.hi)<=0:return (-self).square()
        hi=c.hi([c.binary('mul',a,a,c.UP) for a in (self.lo,self.hi)])
        return Interval(c,c.q(0).lo,hi)
    def monotone(self,f):
        c=self.ctx
        if f in ('sqrt','log') and c.cmp0(self.lo)<0:raise Inconclusive('unresolved positive domain')
        return Interval(c,c.unary(f,self.lo,c.DOWN),c.unary(f,self.hi,c.UP))
    def exp(self):return self.monotone('exp')
    def sqrt(self):return self.monotone('sqrt')
    def log(self):return self.monotone('log')
    def tanh(self):return self.monotone('tanh')
    def trig(self,f):
        # sin/cos are globally 1-Lipschitz. Anchor at the exact lower endpoint.
        c=self.ctx
        delta=c.binary('sub',self.hi,self.lo,c.UP)
        lo=c.binary('sub',c.unary(f,self.lo,c.DOWN),delta,c.DOWN)
        hi=c.binary('add',c.unary(f,self.lo,c.UP),delta,c.UP)
        return Interval(c,c.hi([lo,c.q(-1).lo]),c.lo([hi,c.q(1).hi]))
    def sin(self):return self.trig('sin')
    def cos(self):return self.trig('cos')
    def pow_rational(self,p):
        p=frac(p)
        if p.denominator==1 and p>=0:
            ans=self.cast(1);v=self;n=p.numerator
            while n:
                if n&1:ans=ans*v
                n>>=1
                if n:v=v.square()
            return ans
        return (self.log()*p).exp()

def certify(expression,dtype='float64',precisions=(128,256,512,1024,2048,4096)):
    """expression(Context)->Interval. Raise rather than fabricate unresolved bits."""
    for p in precisions:
        with Context(p) as c:
            try:
                result=expression(c)
                if c.cmp0(result.lo)==0 and c.cmp0(result.hi)==0:return 0.0
                a,b=c.rounded(result.lo,dtype),c.rounded(result.hi,dtype)
                if a==b:
                    value=from_bits(a,dtype)
                    if not math.isfinite(value):raise OverflowError('finite destination overflow')
                    return value
            except Inconclusive:
                continue
    raise Inconclusive('directed endpoints did not determine RN bits within precision cap')

def sqrt_q(q,dtype='float64'):
    q=frac(q)
    if q<0:raise DomainError('negative square root')
    # Detect exact rational square roots, including exact midpoint cases.
    a,b=math.isqrt(q.numerator),math.isqrt(q.denominator)
    if a*a==q.numerator and b*b==q.denominator:return rn(frac(a)/b,dtype)
    return certify(lambda c:c.q(q).sqrt(),dtype)

def exp_q(q,dtype='float64'):
    q=frac(q)
    if not q:return 1.0
    return certify(lambda c:c.q(q).exp(),dtype)

def atan2_q(y,x,dtype='float64',turn=False,half=False):
    y,x=frac(y),frac(x)
    if not x and not y:return 0.0
    if not y:
        if x>0:return 0.0
        return rn(frac(1)/2,dtype) if turn else certify(lambda c:c.pi()/(2 if half else 1),dtype)
    def expr(c):
        yi,xi=c.q(y),c.q(x)
        # Exact arguments fix the quadrant; endpoints cannot cross the cut.
        lows=[c.binary('atan2',yy,xx,c.DOWN) for yy in (yi.lo,yi.hi) for xx in (xi.lo,xi.hi)]
        highs=[c.binary('atan2',yy,xx,c.UP) for yy in (yi.lo,yi.hi) for xx in (xi.lo,xi.hi)]
        v=Interval(c,c.lo(lows),c.hi(highs))
        return v/(2*c.pi()) if turn else v/(2 if half else 1)
    return certify(expr,dtype)
