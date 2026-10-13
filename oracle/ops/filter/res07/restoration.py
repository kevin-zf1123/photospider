from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import sqrt_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

def anscombe_forward(image,*,dtype='float64'):
    h,w=shape(image);out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            v=frac(image[y][x])
            if v<0:raise DomainError('negative count')
            out[y][x]=sqrt_q(4*v+Q(3,2),dtype)
    return out

def anscombe_algebraic(image,*,negative_estimate='clamp',dtype='float64'):
    return generalized_anscombe_algebraic(image,gain=1,offset=0,read_sigma=0,negative_estimate=negative_estimate,dtype=dtype)

def generalized_anscombe_forward(image,*,gain,offset=0,read_sigma=0,dtype='float64'):
    h,w=shape(image);a,b,s=map(frac,(gain,offset,read_sigma))
    if a<=0 or s<0:raise DomainError('GAT calibration')
    return [[sqrt_q(4*max(a*(frac(image[y][x])-b)+Q(3,8)*a*a+s*s,0)/(a*a),dtype) for x in range(w)] for y in range(h)]

def generalized_anscombe_algebraic(image,*,gain,offset=0,read_sigma=0,negative_estimate='clamp',dtype='float64'):
    h,w=shape(image);a,b,s=map(frac,(gain,offset,read_sigma))
    if a<=0 or s<0 or negative_estimate not in ('clamp','error'):raise DomainError('GAT inverse parameters')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            z=frac(image[y][x])
            if z<0:raise DomainError('negative stabilized observation')
            lam=z*z/4-Q(3,8)-(s/a)**2
            if lam<0 and negative_estimate=='error':raise DomainError('negative latent estimate')
            out[y][x]=rn(b+a*max(lam,0),dtype)
    return out

def _mp_q(mp,value):
    value=frac(value);return mp.mpf(value.numerator)/value.denominator

def _poisson_mean_diagnostic(mp,lam,values=None,cap=512):
    weight=mp.exp(-lam);total=mp.mpf('0')
    for k in range(cap):
        value=values[k] if values is not None else 2*mp.sqrt(k+mp.mpf(3)/8)
        total+=weight*value
        if k>lam+30 and abs(weight)<mp.power(10,-mp.mp.dps-5):return total
        weight*=lam/(k+1)
    raise DomainError('diagnostic Poisson truncation cap')

def anscombe_mean_inverse(z,*,dps=70,max_lambda=100):
    """DIAGNOSTIC ONLY: finite series + high precision root, no tail certificate."""
    import mpmath as mp
    zq=frac(z)
    if zq<0:raise DomainError('negative stabilized mean')
    with mp.workdps(dps):
        zz=_mp_q(mp,zq);m0=mp.sqrt(mp.mpf(3)/2)
        if zq*zq<=Q(3,2):root=mp.mpf('0')
        else:
            guess=max(mp.mpf('.01'),zz*zz/4-mp.mpf(3)/8)
            if guess>max_lambda:raise DomainError('mean inverse diagnostic lambda cap')
            fn=lambda lam:_poisson_mean_diagnostic(mp,lam)-zz
            root=mp.findroot(fn,(guess,max(guess*mp.mpf('1.2'),guess+mp.mpf('.1'))))
            if not 0<=root<=max_lambda:raise DomainError('diagnostic root outside cap')
        return {'value':float(root),'decimal':mp.nstr(root,dps),'proof_level':'HighPrecisionDiagnostic','precision_decimal_digits':dps,'tail_certificate':False,'method':'truncated Poisson expectation + high precision secant'}

def generalized_mean_inverse(z,*,gain,offset=0,read_sigma=0,dps=40,max_lambda=12):
    """DIAGNOSTIC ONLY. Positive read-noise quadrature is intentionally costly."""
    import mpmath as mp
    a,b,s,zq=map(frac,(gain,offset,read_sigma,z))
    if a<=0 or s<0 or zq<0:raise DomainError('generalized inverse domain')
    if not s:
        result=anscombe_mean_inverse(zq,dps=dps,max_lambda=max_lambda)
        with mp.workdps(dps):
            result['value']=float(_mp_q(mp,b)+_mp_q(mp,a)*mp.mpf(result['decimal']))
            result['decimal']=mp.nstr(_mp_q(mp,b)+_mp_q(mp,a)*mp.mpf(result['decimal']),dps)
        result['method']='zero-read-noise reduction to Poisson diagnostic';return result
    with mp.workdps(dps):
        rho=_mp_q(mp,s/a);zz=_mp_q(mp,zq);cache={}
        class Values:
            def __getitem__(self,k):
                if k not in cache:
                    threshold=-(k+mp.mpf(3)/8+rho*rho)/rho
                    integrand=lambda t:2*mp.sqrt(max(k+mp.mpf(3)/8+rho*rho+rho*t,0))*mp.exp(-t*t/2)/mp.sqrt(2*mp.pi)
                    cache[k]=mp.quad(integrand,[threshold,max(threshold,mp.mpf(0))+1,mp.inf])
                return cache[k]
        values=Values();m0=values[0]
        if zz<=m0:root=mp.mpf(0)
        else:
            guess=max(mp.mpf('.01'),zz*zz/4-mp.mpf(3)/8-rho*rho)
            if guess>max_lambda:raise DomainError('generalized diagnostic lambda cap')
            fn=lambda lam:_poisson_mean_diagnostic(mp,lam,values,256)-zz
            root=mp.findroot(fn,(guess,guess+mp.mpf('.2')))
            if not 0<=root<=max_lambda:raise DomainError('generalized diagnostic root cap')
        value=_mp_q(mp,b)+_mp_q(mp,a)*root
        return {'value':float(value),'decimal':mp.nstr(value,dps),'proof_level':'HighPrecisionDiagnostic','precision_decimal_digits':dps,'tail_certificate':False,'quadrature_certificate':False,'method':'Gaussian quadrature + truncated Poisson series + secant','quadrature_terms':len(cache)}
