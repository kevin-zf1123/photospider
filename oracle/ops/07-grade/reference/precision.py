"""Independent mathematical oracle: exact rational arithmetic and MPFR enclosures.

Not a kernel, ABI adapter, or production allocation implementation. LP64 only for
ctypes MPFR. No import of retired grade implementations. An unresolved rounding
boundary raises Uncertified; increasing precision is not itself a certificate.
"""
from __future__ import annotations
from dataclasses import dataclass
from fractions import Fraction as F
import ctypes as C
import ctypes.util
import math
import os
import struct

class DomainError(ValueError): pass
class Uncertified(ArithmeticError): pass

def frac(x):
    if isinstance(x, F): return x
    if isinstance(x, int): return F(x)
    if not math.isfinite(x): raise DomainError('consumed nonfinite input')
    return F.from_float(float(x))

def bits(x, dtype=64):
    return int.from_bytes(struct.pack('>f' if dtype==32 else '>d',x),'big')

def from_bits(b, dtype=64):
    return struct.unpack('>f' if dtype==32 else '>d',b.to_bytes(dtype//8,'big'))[0]

def rint(x):
    x=frac(x); q,r=divmod(x.numerator,x.denominator)
    return q+(2*r>x.denominator or (2*r==x.denominator and q%2==1))

def round_fraction(x, dtype=64, negative_zero=False):
    """Direct RN-even to binary32/64, including subnormals; no double rounding."""
    if dtype not in (32,64): raise ValueError('dtype must be 32 or 64')
    x=frac(x); sign=x<0 or (x==0 and negative_zero); x=abs(x)
    if not x: return from_bits(int(sign)<<(dtype-1),dtype)
    p,emin,emax,bias=(24,-126,127,127) if dtype==32 else (53,-1022,1023,1023)
    n,d=x.numerator,x.denominator; e=n.bit_length()-d.bit_length()
    if (n<(d<<e) if e>=0 else (n<<(-e))<d): e-=1
    q=max(e-(p-1),emin-(p-1))
    scaled=F(n,d<<q) if q>=0 else F(n<<(-q),d)
    k=rint(scaled)
    if not k: return from_bits(int(sign)<<(dtype-1),dtype)
    if k < 1<<(p-1): exponent=0; mantissa=k
    else:
        shift=k.bit_length()-p
        if shift>0: k>>=shift; q+=shift
        exponent=q+(p-1)+bias; mantissa=k-(1<<(p-1))
        if exponent>=emax+bias+1:
            return math.copysign(math.inf, -1 if sign else 1)
    return from_bits((int(sign)<<(dtype-1))|(exponent<<(p-1))|mantissa,dtype)

@dataclass(frozen=True)
class Expr:
    op: str
    args: tuple
    def __add__(self,b): return add(self,b)
    def __radd__(self,b): return add(b,self)
    def __sub__(self,b): return add(self,neg(b))
    def __rsub__(self,b): return add(b,neg(self))
    def __mul__(self,b): return mul(self,b)
    def __rmul__(self,b): return mul(b,self)
    def __truediv__(self,b): return div(self,b)
    def __rtruediv__(self,b): return div(b,self)
    def __neg__(self): return neg(self)
    def __pow__(self,b): return power(self,b)

def val(a): return a if isinstance(a,Expr) else frac(a)
def add(a,b):
    a,b=val(a),val(b)
    if not isinstance(a,Expr) and not isinstance(b,Expr): return a+b
    if a==0:return b
    if b==0:return a
    if isinstance(a,Expr) and a.op=='neg' and a.args[0]==b:return F(0)
    if isinstance(b,Expr) and b.op=='neg' and b.args[0]==a:return F(0)
    return Expr('add',(a,b))
def neg(a):
    a=val(a)
    return Expr('neg',(a,)) if isinstance(a,Expr) else -a

def mul(a,b):
    a,b=val(a),val(b)
    if a==0 or b==0:return F(0)
    if a==1:return b
    if b==1:return a
    if not isinstance(a,Expr) and not isinstance(b,Expr):return a*b
    return Expr('mul',(a,b))
def div(a,b):
    a,b=val(a),val(b)
    if b==0:raise DomainError('zero denominator')
    if a==0:return F(0)
    if b==1:return a
    if a==b:return F(1)
    if not isinstance(a,Expr) and not isinstance(b,Expr):return a/b
    return Expr('div',(a,b))
def exp2(a):
    a=val(a)
    if not isinstance(a,Expr) and a.denominator==1 and abs(a)<=16384:
        return F(2)**int(a)
    return Expr('exp2',(a,))
def log2(a):
    a=val(a)
    if not isinstance(a,Expr):
        if a<=0:raise DomainError('log domain')
        n,d=a.numerator,a.denominator
        if n&(n-1)==0 and d&(d-1)==0:return F(n.bit_length()-d.bit_length())
    return Expr('log2',(a,))
def sqrt(a):
    a=val(a)
    if not isinstance(a,Expr):
        if a<0:raise DomainError('sqrt domain')
        n,d=math.isqrt(a.numerator),math.isqrt(a.denominator)
        if n*n==a.numerator and d*d==a.denominator:return F(n,d)
    return Expr('sqrt',(a,))
def power(a,b):
    a,b=val(a),val(b)
    if not isinstance(a,Expr) and a<0:raise DomainError('negative real-power base')
    if b==0:return F(1)
    if b==1:return a
    if a==0:return F(0)
    if a==1:return F(1)
    if not isinstance(a,Expr) and not isinstance(b,Expr) and b.denominator<=64:
        def root(n,k):
            lo,hi=0,1 << ((n.bit_length()+k-1)//k)
            while lo<hi:
                mid=(lo+hi+1)//2
                if mid**k<=n:lo=mid
                else:hi=mid-1
            return lo
        k=b.denominator
        n,d=root(a.numerator,k),root(a.denominator,k)
        if n**k==a.numerator and d**k==a.denominator and abs(b.numerator)<=4096:
            return F(n,d)**b.numerator
    if not isinstance(a,Expr) and not isinstance(b,Expr) and b.denominator==1 and abs(b)<=4096:return a**int(b)
    if b==F(1,2):return sqrt(a)
    return Expr('pow',(a,b))
def exp(a): return F(1) if a==0 else Expr('exp',(val(a),))
def pi():return Expr('pi',())
def acos(a):return F(0) if a==1 else Expr('acos',(val(a),))
def trigpi(a,cos=False):
    a=frac(a)%2
    landmarks={F(0):F(1) if cos else F(0),F(1,2):F(0) if cos else F(1),F(1):F(-1) if cos else F(0),F(3,2):F(0) if cos else F(-1)}
    return landmarks[a] if a in landmarks else Expr('cospi' if cos else 'sinpi',(a,))

class Raw(C.Structure):
    _fields_=[('precision',C.c_long),('sign',C.c_int),('exponent',C.c_long),('limbs',C.POINTER(C.c_ulong))]

class MP:
    down,up,near=3,2,0
    def __init__(self,p):
        if C.sizeof(C.c_long)!=8:raise RuntimeError('MPFR ctypes oracle requires LP64')
        path=os.environ.get('PHOTOSPIDER_ORACLE_MPFR') or ctypes.util.find_library('mpfr')
        if not path:raise RuntimeError('MPFR shared library not found')
        self.lib=C.CDLL(path); self.p=p; self.owned=[]; self.memo={}
        P=C.POINTER(Raw)
        self.lib.mpfr_init2.argtypes=[P,C.c_long]; self.lib.mpfr_clear.argtypes=[P]
        self.lib.mpfr_set_str.argtypes=[P,C.c_char_p,C.c_int,C.c_int]
        self.lib.mpfr_cmp.argtypes=[P,P];self.lib.mpfr_nan_p.argtypes=[P];self.lib.mpfr_get_version.restype=C.c_char_p
        self.version=self.lib.mpfr_get_version().decode()
        if tuple(map(int,self.version.split('.')[:2]))<(4,2):raise RuntimeError('MPFR>=4.2 required')
        for n in ('get_d','get_flt'):
            fn=getattr(self.lib,'mpfr_'+n);fn.argtypes=[P,C.c_int];fn.restype=C.c_double if n=='get_d' else C.c_float
        for n in ('add','sub','mul','div','pow'):
            getattr(self.lib,'mpfr_'+n).argtypes=[P,P,P,C.c_int]
        for n in ('neg','exp2','log2','exp','sqrt','sinpi','cospi','acos'):
            getattr(self.lib,'mpfr_'+n).argtypes=[P,P,C.c_int]
        self.lib.mpfr_const_pi.argtypes=[P,C.c_int]
    def __enter__(self):return self
    def __exit__(self,*_):
        for a in reversed(self.owned):self.lib.mpfr_clear(C.byref(a))
    def new(self,p=None):
        a=Raw();self.lib.mpfr_init2(C.byref(a),p or self.p);self.owned.append(a);return a
    def integer(self,n):
        a=self.new(max(self.p,abs(n).bit_length()+1));self.lib.mpfr_set_str(C.byref(a),str(n).encode(),10,0);return a
    def unary(self,n,a,r):
        o=self.new();getattr(self.lib,'mpfr_'+n)(C.byref(o),C.byref(a),r);return o
    def binary(self,n,a,b,r):
        o=self.new();getattr(self.lib,'mpfr_'+n)(C.byref(o),C.byref(a),C.byref(b),r);return o
    def cmp(self,a,b):
        if self.lib.mpfr_nan_p(C.byref(a)) or self.lib.mpfr_nan_p(C.byref(b)):raise Uncertified('invalid MPFR enclosure endpoint')
        return self.lib.mpfr_cmp(C.byref(a),C.byref(b))
    def output(self,a,dtype):return getattr(self.lib,'mpfr_get_flt' if dtype==32 else 'mpfr_get_d')(C.byref(a),0)
    def minmax(self,items,ismax=False):
        z=items[0]
        for a in items[1:]:
            if (self.cmp(a,z)>0 if ismax else self.cmp(a,z)<0):z=a
        return z
    def bounds(self,e):
        if isinstance(e,Expr):
            key=id(e)
            if key not in self.memo:self.memo[key]=self._bounds(e)
            return self.memo[key]
        return self._bounds(e)
    def _bounds(self,e):
        if not isinstance(e,Expr):
            e=frac(e);n,d=self.integer(e.numerator),self.integer(e.denominator)
            return self.binary('div',n,d,self.down),self.binary('div',n,d,self.up)
        op=e.op
        if op=='pi':
            a,b=self.new(),self.new();self.lib.mpfr_const_pi(C.byref(a),self.down);self.lib.mpfr_const_pi(C.byref(b),self.up);return a,b
        args=[self.bounds(a) for a in e.args];a=args[0]
        if op=='neg':return self.unary('neg',a[1],self.down),self.unary('neg',a[0],self.up)
        if op=='add':
            b=args[1];return self.binary('add',a[0],b[0],self.down),self.binary('add',a[1],b[1],self.up)
        if op in ('mul','div','pow'):
            b=args[1];zero=self.integer(0)
            if op=='div' and self.cmp(b[0],zero)<=0 and self.cmp(b[1],zero)>=0:raise Uncertified('denominator enclosure includes zero')
            if op=='pow' and self.cmp(a[0],zero)<0:raise Uncertified('power base enclosure negative')
            lo=[self.binary(op,x,y,self.down) for x in a for y in b]
            hi=[self.binary(op,x,y,self.up) for x in a for y in b]
            return self.minmax(lo),self.minmax(hi,True)
        if op in ('sinpi','cospi'):
            # Include all stationary points crossed by an argument enclosure.
            low=[self.unary(op,x,self.down) for x in a]
            high=[self.unary(op,x,self.up) for x in a]
            stationary=((F(1,2),1),(F(3,2),-1)) if op=='sinpi' else ((F(0),1),(F(1),-1),(F(2),1))
            for where,value in stationary:
                z=self.bounds(where)[0]
                if self.cmp(a[0],z)<=0 and self.cmp(a[1],z)>=0:
                    low.append(self.integer(value));high.append(self.integer(value))
            return self.minmax(low),self.minmax(high,True)
        if op=='acos':return self.unary(op,a[1],self.down),self.unary(op,a[0],self.up)
        if op in ('exp2','log2','exp','sqrt'):
            return self.unary(op,a[0],self.down),self.unary(op,a[1],self.up)
        raise ValueError(op)

PRECISIONS=(128,256,512,1024,2048,4096,8192)
def rn(e,dtype=64):
    if not isinstance(e,Expr):return round_fraction(e,dtype)
    for p in PRECISIONS:
        with MP(p) as m:
            try:a,b=m.bounds(e)
            except Uncertified:continue
            lo,hi=m.output(a,dtype),m.output(b,dtype)
            if math.isnan(lo) or math.isnan(hi):continue
            if bits(lo,dtype)==bits(hi,dtype):
                return lo
    raise Uncertified('interval endpoints did not certify destination rounding')

def compare(a,b=0):
    a,b=val(a),val(b)
    if a==b:return 0
    if not isinstance(a,Expr) and not isinstance(b,Expr):return (a>b)-(a<b)
    e=add(a,neg(b))
    for p in PRECISIONS:
        with MP(p) as m:
            try:lo,hi=m.bounds(e)
            except Uncertified:continue
            z=m.integer(0)
            try:
                if m.cmp(lo,z)>0:return 1
                if m.cmp(hi,z)<0:return -1
                if m.cmp(lo,z)==0 and m.cmp(hi,z)==0:return 0
            except Uncertified:continue
    raise Uncertified('comparison undecided; no epsilon substitution')

def clamp(x,lo=0,hi=1):
    if compare(x,lo)<=0:return val(lo)
    if compare(x,hi)>=0:return val(hi)
    return val(x)

def smooth(x):
    t=clamp(x);return t*t*(3-2*t)

def spow(x,p):
    x=val(x);s=compare(x)
    return 0 if s==0 else s*power(x if s>0 else -x,p)

def finite_copy(x,dtype=64):
    f=frac(x);return round_fraction(f,dtype,negative_zero=math.copysign(1,float(x))<0 and f==0)
