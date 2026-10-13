from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac

def gabor_kernel(*,frequency,theta=0,phase=0,sigma=1,aspect=1,radius_y=1,radius_x=1,dc='keep',normalization='none'):
    f,t,p,s,g=map(frac,(frequency,theta,phase,sigma,aspect))
    if not 0<=f<=Q(1,2) or min(s,g)<=0 or min(radius_y,radius_x)<0:raise DomainError('Gabor parameters')
    if dc not in ('keep','remove') or normalization not in ('none','l1_envelope'):raise DomainError('Gabor profile')
    pts=[(y,x) for y in range(-radius_y,radius_y+1) for x in range(-radius_x,radius_x+1)]
    if len(pts)>81:raise DomainError('oracle Gabor cap=81 taps, not runtime limit')
    def expression(ctx,target,imag):
        ct,st=ctx.q(t).cos(),ctx.q(t).sin()
        entries=[];envelopes=[]
        for y,x in pts:
            xx=x*ct+y*st;yy=-x*st+y*ct
            env=(-(xx.square()+g*g*yy.square())/(2*s*s)).exp()
            ph=2*ctx.pi()*f*xx+p
            entries.append(env*(ph.sin() if imag else ph.cos()));envelopes.append(env)
        value=entries[target]
        if dc=='remove':value=value-sum(entries,ctx.q(0))/len(pts)
        if normalization=='l1_envelope':value=value/sum(envelopes,ctx.q(0))
        return value
    out=[]
    for imag in (False,True):
        vals=[]
        for idx,(y,x) in enumerate(pts):
            # Exact zero-phase, zero-frequency imaginary component.
            if imag and f==0 and p==0:v=0.0
            else:v=certify(lambda c:expression(c,idx,imag))
            vals.append(v)
        width=2*radius_x+1;out.append([vals[i:i+width] for i in range(0,len(vals),width)])
    return tuple(out)
