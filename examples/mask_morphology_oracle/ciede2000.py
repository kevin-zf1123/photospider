"""Directed-MPFR interval certification of the explicit CIEDE2000 selector.

Angles are degrees; sinpi/cospi/atan2pi preserve exact rational angle scales.
Unresolved comparisons/rounding fail rather than returning an approximation.
"""
from fractions import Fraction as Q
import math
from exact import OracleError, finite, param, rn, from_bits
from gaussian import MPFR, fraction_bounds


class Retry(Exception): pass


class Interval:
    def __init__(self,m,lo,hi=None):self.m,self.lo,self.hi=m,lo,lo if hi is None else hi
    @classmethod
    def q(cls,m,q):return cls(m,*fraction_bounds(m,Q(q)))
    def as_interval(self,x):return x if isinstance(x,Interval) else Interval.q(self.m,x)
    def __add__(self,x):
        x=self.as_interval(x);m=self.m
        return Interval(m,m.binary('add',self.lo,x.lo,m.downward),m.binary('add',self.hi,x.hi,m.upward))
    __radd__=__add__
    def __neg__(self):
        m=self.m;return Interval(m,m.unary('neg',self.hi,m.downward),m.unary('neg',self.lo,m.upward))
    def __sub__(self,x):return self+-self.as_interval(x)
    def __rsub__(self,x):return self.as_interval(x)+-self
    def __mul__(self,x):
        x=self.as_interval(x);m=self.m
        low=[m.binary('mul',a,b,m.downward) for a in (self.lo,self.hi) for b in (x.lo,x.hi)]
        high=[m.binary('mul',a,b,m.upward) for a in (self.lo,self.hi) for b in (x.lo,x.hi)]
        return Interval(m,minimum(m,low),maximum(m,high))
    __rmul__=__mul__
    def __truediv__(self,x):
        x=self.as_interval(x);m=self.m;zero=m.integer(0)
        if m.compare(x.lo,zero)<=0 and m.compare(x.hi,zero)>=0:raise Retry()
        one=m.integer(1)
        return self*Interval(m,m.binary('div',one,x.hi,m.downward),m.binary('div',one,x.lo,m.upward))
    def __pow__(self,n):
        if n<0:return self.as_interval(1)/(self**-n)
        if n==0:return self.as_interval(1)
        if n==2:
            m=self.m;zero=m.integer(0)
            ends=[self.lo,self.hi]
            lo=zero if m.compare(self.lo,zero)<=0<=m.compare(self.hi,zero) else minimum(m,[m.binary('mul',x,x,m.downward) for x in ends])
            return Interval(m,lo,maximum(m,[m.binary('mul',x,x,m.upward) for x in ends]))
        return (self**(n//2))**2*(self if n%2 else 1)
    def sqrt(self):
        m=self.m;zero=m.integer(0)
        if m.compare(self.hi,zero)<0:raise Retry()
        return Interval(m,m.unary('sqrt',maximum(m,[zero,self.lo]),m.downward),m.unary('sqrt',self.hi,m.upward))
    def exp(self):
        m=self.m;return Interval(m,m.unary('exp',self.lo,m.downward),m.unary('exp',self.hi,m.upward))
    def le(self,q):
        x=self.as_interval(q);m=self.m
        if m.compare(self.hi,x.lo)<=0:return True
        if m.compare(self.lo,x.hi)>0:return False
        raise Retry()
    def absolute(self):
        m=self.m;z=m.integer(0)
        if m.compare(self.lo,z)>=0:return self
        if m.compare(self.hi,z)<=0:return -self
        return Interval(m,z,maximum(m,[m.unary('neg',self.lo,m.upward),self.hi]))
    def trig_degrees(self,cosine=False):
        a=self/180;m=self.m;fun='cospi' if cosine else 'sinpi'
        low=[m.unary(fun,x,m.downward) for x in (a.lo,a.hi)]
        high=[m.unary(fun,x,m.upward) for x in (a.lo,a.hi)]
        # All CIEDE2000 trig arguments are inside [-10*pi,10*pi].
        if not a.le(10) or not (-a).le(10):raise Retry()
        for k in range(-10,11):
            at=Interval.q(m,Q(k) if cosine else Q(2*k+1,2))
            if m.compare(at.hi,a.lo)>=0 and m.compare(at.lo,a.hi)<=0:
                v=m.integer(1 if k%2==0 else -1);low.append(v);high.append(v)
        return Interval(m,minimum(m,low),maximum(m,high))


def minimum(m,values):
    best=values[0]
    for x in values[1:]:
        if m.compare(x,best)<0:best=x
    return best


def maximum(m,values):
    best=values[0]
    for x in values[1:]:
        if m.compare(x,best)>0:best=x
    return best


def hue(m,ap,b,raw_a,raw_b):
    if not raw_b:return Interval.q(m,180 if raw_a<0 else 0)
    if not raw_a:return Interval.q(m,90 if raw_b>0 else 270)
    low=[m.binary('atan2pi',y,x,m.downward) for y in (b.lo,b.hi) for x in (ap.lo,ap.hi)]
    high=[m.binary('atan2pi',y,x,m.upward) for y in (b.lo,b.hi) for x in (ap.lo,ap.hi)]
    h=Interval(m,minimum(m,low),maximum(m,high))*180
    return h+360 if raw_b<0 else h


def distance_interval(m,p,q,kl,kc,kh):
    I=lambda x:Interval.q(m,x)
    l1,a1,b1=map(I,p);l2,a2,b2=map(I,q)
    c1=(a1*a1+b1*b1).sqrt();c2=(a2*a2+b2*b2).sqrt()
    cb=(c1+c2)/2
    g=(1-((cb**7)/(cb**7+25**7)).sqrt())/2
    ap1=(1+g)*a1;ap2=(1+g)*a2
    cp1=(ap1**2+b1**2).sqrt();cp2=(ap2**2+b2**2).sqrt()
    h1=hue(m,ap1,b1,p[1],p[2]);h2=hue(m,ap2,b2,q[1],q[2])
    neutral=(p[1]==p[2]==0 or q[1]==q[2]==0)
    dl=l2-l1;dc=cp2-cp1
    if neutral:dh=I(0);hm=h1+h2
    else:
        diff=h2-h1
        cross=p[1]*q[2]-p[2]*q[1]
        dot=p[1]*q[1]+p[2]*q[2]
        # G scales both a axes equally. Exact collinearity decides angular ties.
        if cross==0 and dot>0:diff=I(0);small=True
        elif cross==0 and dot<0:
            diff=I(180 if (h1-h2).le(0) else -180);small=True
        else:small=diff.absolute().le(180)
        dh=diff if small else diff-360 if (-diff).le(0) else diff+360
        if small:hm=(h1+h2)/2
        else:
            # Opposite b signs and identical a/b reflection give exact sum 360.
            sum360=p[2]*q[2]<0 and p[1]*q[2]+p[2]*q[1]==0
            hs=I(360) if sum360 else h1+h2
            hm=(hs-360)/2 if sum360 or not hs.le(360) else (hs+360)/2
    dhbig=2*(cp1*cp2).sqrt()*(dh/2).trig_degrees()
    lm=(l1+l2)/2;cm=(cp1+cp2)/2
    t=1-Q(17,100)*(hm-30).trig_degrees(True)+Q(24,100)*(2*hm).trig_degrees(True)+Q(32,100)*(3*hm+6).trig_degrees(True)-Q(20,100)*(4*hm-63).trig_degrees(True)
    theta=30*(-((hm-275)/25)**2).exp()
    rc=2*((cm**7)/(cm**7+25**7)).sqrt()
    sl=1+Q(15,1000)*(lm-50)**2/(20+(lm-50)**2).sqrt()
    sc=1+Q(45,1000)*cm;sh=1+Q(15,1000)*cm*t
    rt=-(2*theta).trig_degrees()*rc
    vl=dl/(kl*sl);vc=dc/(kc*sc);vh=dhbig/(kh*sh)
    return (vl**2+vc**2+vh**2+rt*vc*vh).sqrt()


def certify(p,q,kl,kc,kh,dtype,inner=None,outer=None,curve='linear'):
    # Inputs here use physical L*,a*,b* as exact rational values.
    if p==q:
        return rn(0 if inner is None else 1,dtype)
    # Pure neutral pair centered at L*=50 has SL=1, exact rational distance.
    exact=None
    if p[1]==p[2]==q[1]==q[2]==0 and p[0]+q[0]==100:exact=abs(p[0]-q[0])/kl
    if exact is not None:
        if inner is None:return rn(exact,dtype)
        from reference import radial
        return radial(exact,inner,outer,curve,dtype)
    size=max(max(abs(x.numerator).bit_length(),x.denominator.bit_length()) for x in (*p,*q,kl,kc,kh,*(() if inner is None else (inner,outer))))
    precision=1<<(max(128,size+16)-1).bit_length()
    while precision<=8192:
        try:
            with MPFR(precision) as m:
                d=distance_interval(m,p,q,kl,kc,kh)
                if inner is not None:
                    if d.le(inner):return 1.
                    if inner==outer:return 0.
                    if (-d).le(-outer):return 0.
                    u=(d-inner)/(outer-inner)
                    response=1-u if curve=='linear' else 1-u*u*(3-2*u)
                else:response=d
                width=4 if dtype=='float32' else 8
                lo,hi=m.bits(response.lo,width),m.bits(response.hi,width)
                if lo==hi:
                    result=from_bits(lo,dtype)
                    if not math.isfinite(result):raise OracleError('finite result overflow','ArithmeticOverflow')
                    return result
        except Retry:pass
        precision*=2
    raise OracleError('CIEDE2000 interval not certified by 8192 bits','Uncertified')


def selector(image,target,channels,kL,kC,kH,inner,outer,curve,dtype):
    from reference import tensor,channel_indices,radii
    a=tensor(image,dtype);cs=channel_indices(channels,len(a))
    if len(cs)!=3 or len(target)!=3:raise OracleError('Lab group requires three coordinates','TypeMismatch')
    t=[param(v) for v in target];t[0]*=100
    kl,kc,kh=[param(v,'CIE factor',0,True) for v in (kL,kC,kH)]
    i,o=radii(inner,outer,curve)
    if len(a[0])*len(a[0][0])>64:raise OracleError('manual Lab2000 oracle cap','OracleCapacity')
    out=[]
    for y in range(len(a[0])):
        row=[]
        for x in range(len(a[0][0])):
            p=[finite(a[c][y][x]) for c in cs];p[0]*=100
            row.append(certify(p,t,kl,kc,kh,dtype,i,o,curve))
        out.append(row)
    return out
