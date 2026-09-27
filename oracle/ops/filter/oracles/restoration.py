"""Independent tiny-image restoration references.

Rational solvers/explicit RN stages are exact references. Only the two explicitly
marked mean-domain inverse routines use mpmath diagnostics without proof of
truncation/integration error. No BM3D/SMAA engine is substituted here.
"""
from __future__ import annotations
import copy
from functools import lru_cache
from .core import Q,frac,rn,zeros,shape,same_shape,sample,coordinate,DomainError,solve_linear,median_exact
from .bounds import Context,Interval,certify,sqrt_q,exp_q
from .spatial import separable,clipped_box,window_samples
from .transcend import gaussian_kernel

def detail_gain(base,detail,*,gain=1,dtype='float64'):
    h,w=same_shape(base,detail);g=frac(gain)
    if not g:return copy.deepcopy(base)
    return [[rn(frac(base[y][x])+g*frac(detail[y][x]),dtype) for x in range(w)] for y in range(h)]

def unsharp(image,*,amount=0,threshold=0,sigma_y=1,sigma_x=1,radius_y=1,radius_x=1,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image);a,t=frac(amount),frac(threshold)
    if t<0:raise DomainError('threshold')
    if not a:return copy.deepcopy(image)
    kx,ky=gaussian_kernel(sigma_x,radius_x),gaussian_kernel(sigma_y,radius_y)
    b=separable(image,kx,ky,(radius_y,radius_x),normalization='sum',boundary=boundary,cval=cval,dtype=dtype)
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            v=frac(image[y][x]);d=frac(rn(v-frac(b[y][x]),dtype))
            out[y][x]=rn(v+a*d,dtype) if abs(d)>t else image[y][x]
    return out

@lru_cache(None)
def _normal_quantile_bracket(precision):
    # Prove erf(c/sqrt(2)) straddles 1/2 by directed MPFR evaluation.
    lo,hi=Q(),Q(1)
    with Context(precision+96) as ctx:
        for _ in range(precision+8):
            m=(lo+hi)/2;v=(ctx.q(m)/ctx.q(2).sqrt()).monotone('erf');half=ctx.q(Q(1,2))
            if ctx.cmp(v.hi,half.lo)<0:lo=m
            elif ctx.cmp(v.lo,half.hi)>0:hi=m
            else:raise ArithmeticError('normal quantile bracket unresolved')
    return lo,hi

def noise_mad(image,mask,*,min_samples=1):
    h,w=same_shape(image,mask)
    if h<2 or w<2 or min_samples<1:raise DomainError('MAD shape/sample count')
    if any(v not in (0,1) for row in mask for v in row):raise DomainError('mask binary')
    residual=[]
    for y in range(h-1):
        for x in range(w-1):
            if all(mask[y+j][x+i] for j in (0,1) for i in (0,1)):
                residual.append(rn((frac(image[y][x])-frac(image[y][x+1])-frac(image[y+1][x])+frac(image[y+1][x+1]))/2))
    if len(residual)<min_samples:raise DomainError('insufficient selected residuals')
    m=frac(rn(median_exact(residual)));mad=median_exact([abs(frac(v)-m) for v in residual])
    if not mad:return 0.0
    def expr(c):
        lo,hi=_normal_quantile_bracket(c.precision+16)
        return c.q(mad)/Interval(c,c.q(lo).lo,c.q(hi).hi)
    return certify(expr)

def calibrated_variance(image,*,gain,offset=0,read_sigma=0,below_offset='clamp',dtype='float64'):
    h,w=shape(image);a,b,s=map(frac,(gain,offset,read_sigma))
    if a<=0 or s<0 or below_offset not in ('clamp','error'):raise DomainError('calibration')
    variance=zeros(h,w);sigma=zeros(h,w)
    for y in range(h):
        for x in range(w):
            v=frac(image[y][x])-b
            if v<0 and below_offset=='error':raise DomainError('below offset')
            v=a*max(v,0)+s*s
            variance[y][x]=rn(v,dtype);sigma[y][x]=sqrt_q(v,dtype)
    return variance,sigma

def nlm(image,guide,*,patch_radius=1,search_radius=1,h_parameter=1,metric_scale=None,noise_sigma=None,dtype='float64'):
    h,w=same_shape(image,guide);hp=frac(h_parameter)
    if min(patch_radius,search_radius)<0 or hp<=0:raise DomainError('NLM parameters')
    sigma=Q() if noise_sigma is None else frac(noise_sigma)
    if sigma<0:raise DomainError('sigma')
    if search_radius==0:return copy.deepcopy(image)
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if not 1<=gdim<=4 or len(scales)!=gdim or min(scales)<=0:raise DomainError('guide metric')
    def g(y,x):
        yy,xx=coordinate(y,x,h,w,'reflect_half')
        return [frac(guide[yy][xx][c] if vector else guide[yy][xx])*scales[c] for c in range(gdim)]
    out=zeros(h,w);npatch=(2*patch_radius+1)**2*gdim
    for y in range(h):
        for x in range(w):
            num=den=Q()
            for j,i in clipped_box(y,x,h,w,search_radius,search_radius):
                if (j,i)==(y,x):weight=Q(1)
                else:
                    distance=Q()
                    for dy in range(-patch_radius,patch_radius+1):
                        for dx in range(-patch_radius,patch_radius+1):
                            a,b=g(y+dy,x+dx),g(j+dy,i+dx)
                            distance+=sum(((v-u)**2 for v,u in zip(a,b)),Q())
                    distance/=npatch
                    if noise_sigma is not None:distance=max(distance-2*sigma*sigma,0)
                    weight=frac(exp_q(-distance/(hp*hp)))
                if weight:num+=weight*frac(image[j][i]);den+=weight
            out[y][x]=rn(num/den,dtype)
    return out

def _flatten(a):return [frac(v) for row in a for v in row]
def _reshape(v,h,w):return [v[y*w:(y+1)*w] for y in range(h)]
def _matvec(a,x):return [sum((q*v for q,v in zip(row,x)),Q()) for row in a]
def _transpose(a):return [list(row) for row in zip(*a)]
def _matmul(a,b):
    bt=_transpose(b)
    return [[sum((x*y for x,y in zip(row,col)),Q()) for col in bt] for row in a]

def imaging_matrix(h,w,kernel,anchor=(0,0),boundary='constant'):
    if h*w>64:raise DomainError('matrix oracle cap 64 pixels')
    kh,kw=shape(kernel);ay,ax=anchor
    if boundary not in ('constant','wrap') or not(0<=ay<kh and 0<=ax<kw):raise DomainError('imaging geometry')
    k=[[frac(v) for v in row] for row in kernel]
    if any(v<0 for row in k for v in row):raise DomainError('negative PSF')
    total=sum((sum(row,Q()) for row in k),Q())
    if total<=0:raise DomainError('empty PSF')
    a=[[Q() for _ in range(h*w)] for _ in range(h*w)]
    for y in range(h):
        for x in range(w):
            for j in range(kh):
                for i in range(kw):
                    if not k[j][i]:continue
                    p=coordinate(y+ay-j,x+ax-i,h,w,boundary)
                    if p is not None:a[y*w+x][p[0]*w+p[1]]+=k[j][i]/total
    return a

def forward_adjoint(image,kernel,anchor=(0,0),*,probe=None,boundary='constant'):
    h,w=shape(image);a=imaging_matrix(h,w,kernel,anchor,boundary);x=_flatten(image)
    result={'matrix':a,'forward':_reshape(_matvec(a,x),h,w)}
    if probe is not None:
        same_shape(image,probe);z=_flatten(probe)
        result['adjoint']=_reshape(_matvec(_transpose(a),z),h,w)
        result['dot_forward']=sum((u*v for u,v in zip(_matvec(a,x),z)),Q())
        result['dot_adjoint']=sum((u*v for u,v in zip(x,_matvec(_transpose(a),z))),Q())
    return result

def rl(observation,psf,initial,background,mask,*,iterations=1,epsilon=0,anchor=(0,0),boundary='constant',dtype='float64',request_estimate=True,request_unobserved=True):
    h,w=same_shape(observation,initial,background,mask);eps=frac(epsilon)
    if iterations<0 or eps<0:raise DomainError('RL iterations/epsilon')
    if iterations==0 and not request_unobserved:return copy.deepcopy(initial),None
    a=imaging_matrix(h,w,psf,anchor,boundary);at=_transpose(a);m=_flatten(mask)
    if any(not 0<=v<=1 for v in m):raise DomainError('mask range')
    sensitivity=_matvec(at,m);unobserved=_reshape([int(v==0) for v in sensitivity],h,w)
    if not request_estimate:return None,unobserved
    if iterations==0:return copy.deepcopy(initial),unobserved
    x=_flatten(initial);b=_flatten(background)
    if any(v<0 for v in x+b):raise DomainError('negative RL initial/background')
    raw_y=[v for row in observation for v in row];y=[frac(v) if mm else Q() for v,mm in zip(raw_y,m)]
    if any(v<0 for v in y):raise DomainError('negative observed sample')
    for _ in range(iterations):
        v=[frac(rn(u+bb,dtype)) for u,bb in zip(_matvec(a,x),b)]
        r=[]
        for yy,mm,vv in zip(y,m,v):
            if not mm:r.append(Q());continue
            den=vv+eps
            if not den:
                if yy:raise DomainError('positive observation with zero prediction')
                r.append(Q())
            else:r.append(frac(rn(mm*yy/den,dtype)))
        t=[frac(rn(v,dtype)) for v in _matvec(at,r)]
        x=[frac(rn(xx*tt/ss,dtype)) if ss else xx for xx,tt,ss in zip(x,t,sensitivity)]
    return _reshape([rn(v,dtype) for v in x],h,w),(unobserved if request_unobserved else None)

def _gradient_matrix(h,w,dx=1,dy=1,periodic=False):
    dx,dy=frac(dx),frac(dy);n=h*w;k=[[Q() for _ in range(n)] for _ in range(2*n)]
    for y in range(h):
        for x in range(w):
            i=y*w+x
            if y+1<h or periodic:
                k[i][((y+1)%h)*w+x]+=1/dy;k[i][i]-=1/dy
            if x+1<w or periodic:
                k[n+i][y*w+(x+1)%w]+=1/dx;k[n+i][i]-=1/dx
    return k

def tv_cp(image,*,lam,iterations=100,tau=0.24,sigma=0.24,theta=1,dx=1,dy=1,isotropic=True,dtype='float64'):
    h,w=shape(image);lam,tau,sigma,theta,dx,dy=map(frac,(lam,tau,sigma,theta,dx,dy))
    if lam<0 or min(tau,sigma,dx,dy)<=0 or not 0<=theta<=1 or iterations<0:raise DomainError('TV parameters')
    if tau*sigma*4*(1/(dx*dx)+1/(dy*dy))>=1:raise DomainError('TV step bound')
    if not lam or not iterations:return copy.deepcopy(image)
    if h*w>36:raise DomainError('TV exact matrix oracle cap 36 pixels')
    n=h*w;k=_gradient_matrix(h,w,dx,dy);kt=_transpose(k);f=_flatten(image);u=f[:];ubar=f[:];p=[Q()]*(2*n)
    for _ in range(iterations):
        ph=[frac(rn(a+sigma*b,dtype)) for a,b in zip(p,_matvec(k,ubar))]
        pn=[Q()]*(2*n)
        for i in range(n):
            a,b=ph[i],ph[n+i]
            if isotropic:
                norm2=a*a+b*b
                if norm2<=lam*lam:pn[i],pn[n+i]=a,b
                else:
                    pn[i]=Q() if not a else frac(certify(lambda c:c.q(lam*a)/c.q(norm2).sqrt(),dtype))
                    pn[n+i]=Q() if not b else frac(certify(lambda c:c.q(lam*b)/c.q(norm2).sqrt(),dtype))
            else:pn[i],pn[n+i]=frac(rn(min(max(a,-lam),lam),dtype)),frac(rn(min(max(b,-lam),lam),dtype))
        un=[frac(rn((a-tau*b+tau*ff)/(1+tau),dtype)) for a,b,ff in zip(u,_matvec(kt,pn),f)]
        ubar=[frac(rn(a+theta*(a-b),dtype)) for a,b in zip(un,u)];u,p=un,pn
    return _reshape([rn(v,dtype) for v in u],h,w)

def diffusion(image,*,kappa,dt=Q(1,4),steps=1,dx=1,dy=1,exponential=False,dtype='float64'):
    h,w=shape(image);kap,dt,dx,dy=map(frac,(kappa,dt,dx,dy))
    if min(kap,dt,dx,dy)<=0 or steps<0 or dt>1/(2*(1/(dx*dx)+1/(dy*dy))):raise DomainError('diffusion parameters/stability')
    if not steps:return copy.deepcopy(image)
    u=_flatten(image)
    edges=[]
    for y in range(h):
        for x in range(w):
            i=y*w+x
            if x+1<w:edges.append((i,i+1,dx))
            if y+1<h:edges.append((i,i+w,dy))
    for _ in range(steps):
        flux=[Q()]*(h*w)
        for i,j,spacing in edges:
            ratio=((u[j]-u[i])/(spacing*kap))**2
            c=frac(exp_q(-ratio)) if exponential else frac(rn(1/(1+ratio)))
            f=c*(u[j]-u[i])/(spacing*spacing);flux[i]+=f;flux[j]-=f
        u=[frac(rn(v+dt*f,dtype)) for v,f in zip(u,flux)]
    return _reshape([rn(v,dtype) for v in u],h,w)

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

def _solve_psd(a,b,singular='error'):
    """Rational PSD solve; 'zero' selects exact minimum Euclidean norm solution."""
    if singular not in ('error','zero'):raise DomainError('singular policy')
    n=len(b);m=[list(row)+[b[i]] for i,row in enumerate(a)];piv=[];row=0
    for col in range(n):
        p=next((r for r in range(row,n) if m[r][col]),None)
        if p is None:continue
        m[row],m[p]=m[p],m[row];v=m[row][col];m[row]=[x/v for x in m[row]]
        for r in range(n):
            if r!=row and m[r][col]:
                v=m[r][col];m[r]=[x-v*y for x,y in zip(m[r],m[row])]
        piv.append(col);row+=1
    if any(not any(r[:n]) and r[n] for r in m):raise DomainError('inconsistent system')
    if row<n and singular=='error':raise DomainError('singular frequency system')
    x=[Q()]*n
    for r,c in enumerate(piv):x[c]=m[r][n]
    free=[j for j in range(n) if j not in piv]
    if free:
        null=[]
        for j in free:
            v=[Q()]*n;v[j]=1
            for r,c in enumerate(piv):v[c]=-m[r][j]
            null.append(v)
        gram=[[sum((a*b for a,b in zip(u,v)),Q()) for v in null] for u in null]
        rhs=[-sum((a*b for a,b in zip(v,x)),Q()) for v in null]
        t=solve_linear(gram,rhs)
        x=[x[i]+sum((tt*v[i] for tt,v in zip(t,null)),Q()) for i in range(n)]
    return x

def tikhonov(observation,psf,*,lam,regularizer='gradient',dx=1,dy=1,anchor=(0,0),singular='error',dtype='float64'):
    h,w=shape(observation);n=h*w;lam,dx,dy=map(frac,(lam,dx,dy))
    if n>36 or lam<0 or min(dx,dy)<=0:raise DomainError('Tikhonov parameters/oracle cap')
    a=imaging_matrix(h,w,psf,anchor,'wrap');at=_transpose(a);normal=_matmul(at,a)
    if regularizer=='identity':r=[[Q(int(i==j)) for j in range(n)] for i in range(n)]
    elif regularizer in ('gradient','laplacian'):
        g=_gradient_matrix(h,w,dx,dy,True);r=_matmul(_transpose(g),g)
        if regularizer=='laplacian':r=_matmul(r,r)
    else:raise DomainError('regularizer')
    normal=[[normal[i][j]+lam*r[i][j] for j in range(n)] for i in range(n)]
    x=_solve_psd(normal,_matvec(at,_flatten(observation)),singular)
    return _reshape([rn(v,dtype) for v in x],h,w)

def wiener(observation,psf,*,nsr,anchor=(0,0),singular='error',dtype='float64'):
    return tikhonov(observation,psf,lam=nsr,regularizer='identity',anchor=anchor,singular=singular,dtype=dtype)

def simplex_projection(v):
    v=list(map(frac,v))
    if not v:raise DomainError('empty simplex')
    ordered=sorted(enumerate(v),key=lambda x:(-x[1],x[0]));total=Q();rho=0;theta=Q()
    for j,(_,value) in enumerate(ordered,1):
        total+=value;t=(total-1)/j
        if value>t:rho=j;theta=t
    if not rho:raise AssertionError('simplex projection has no active entry')
    return [max(x-theta,0) for x in v]

def blind_psf(observation,initial_image,initial_psf,support,*,iterations=1,step_image=0.1,step_psf=0.01,image_l2=0,psf_l2=0,anchor=(0,0),dtype='float64'):
    h,w=same_shape(observation,initial_image);kh,kw=same_shape(initial_psf,support)
    ex,ek,lx,lk=map(frac,(step_image,step_psf,image_l2,psf_l2))
    if min(ex,ek)<=0 or min(lx,lk)<0 or iterations<0:raise DomainError('blind parameters')
    if any(v not in (0,1) for row in support for v in row) or not any(v for row in support for v in row):raise DomainError('PSF support')
    if iterations==0:return copy.deepcopy(initial_image),[[rn(v,dtype) for v in row] for row in initial_psf]
    y,x,k=_flatten(observation),_flatten(initial_image),_flatten(initial_psf);active=[i for i,v in enumerate(sum(support,[])) if v]
    if min(x+k)<0 or sum(k,Q())!=1 or any(k[i] for i in range(len(k)) if i not in active):raise DomainError('initial blind constraints')
    for _ in range(iterations):
        a=imaging_matrix(h,w,_reshape(k,kh,kw),anchor,'wrap');at=_transpose(a)
        r=[frac(rn(v-yy,dtype)) for v,yy in zip(_matvec(a,x),y)]
        x=[frac(rn(max(0,xx-ex*(g+lx*xx)),dtype)) for xx,g in zip(x,_matvec(at,r))]
        r=[frac(rn(v-yy,dtype)) for v,yy in zip(_matvec(a,x),y)]
        xx=_reshape(x,h,w);grad=[]
        for j in range(kh):
            for i in range(kw):
                g=sum((r[yy*w+xxi]*sample(xx,yy+anchor[0]-j,xxi+anchor[1]-i,'wrap') for yy in range(h) for xxi in range(w)),Q())+lk*k[j*kw+i]
                grad.append(frac(rn(g,dtype)))
        projected=simplex_projection([k[i]-ek*grad[i] for i in active]);k=[Q()]*(kh*kw)
        for i,value in zip(active,projected):k[i]=value # exact rational constrained state
    return _reshape([rn(v,dtype) for v in x],h,w),_reshape([rn(v,dtype) for v in k],kh,kw)

def low_contrast_smoothing(image,mask,*,radius=1,range_threshold=1,strength=Q(1,2),dtype='float64'):
    h,w=same_shape(image,mask);tau,s=frac(range_threshold),frac(strength)
    if radius<1 or tau<=0 or not 0<=s<=1:raise DomainError('deband parameters')
    if not s:return copy.deepcopy(image)
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            m=frac(mask[y][x])
            if not 0<=m<=1:raise DomainError('mask range')
            if not m:out[y][x]=image[y][x];continue
            a=list(map(frac,window_samples(image,y,x,2*radius+1,2*radius+1,(radius,radius),'reflect_half')))
            center=frac(image[y][x])
            out[y][x]=rn(center+m*s*(sum(a,Q())/len(a)-center),dtype) if max(a)-min(a)<=tau else image[y][x]
    return out

def pairwise_block_boundary_smoothing(image,mask,*,block_height=8,block_width=8,origin_y=0,origin_x=0,threshold=1,strength=Q(1,4),dtype='float64'):
    h,w=same_shape(image,mask);tau,s=frac(threshold),frac(strength)
    if min(block_height,block_width)<2 or tau<=0 or not 0<=s<=Q(1,2):raise DomainError('deblock parameters')
    m=[[frac(v) for v in row] for row in mask]
    if any(not 0<=v<=1 for row in m for v in row):raise DomainError('mask range')
    current=copy.deepcopy(image)
    for axis in (1,0):
        out=copy.deepcopy(current)
        for y in range(h):
            for x in range(w):
                if axis==1:
                    if x==0 or (x-origin_x)%block_width:continue
                    p,q=(y,x-1),(y,x)
                else:
                    if y==0 or (y-origin_y)%block_height:continue
                    p,q=(y-1,x),(y,x)
                if not s or min(m[p[0]][p[1]],m[q[0]][q[1]])==0:continue
                a,b=frac(current[p[0]][p[1]]),frac(current[q[0]][q[1]])
                if abs(b-a)<=tau:
                    d=s*min(m[p[0]][p[1]],m[q[0]][q[1]])*(b-a)
                    out[p[0]][p[1]]=rn(a+d,dtype);out[q[0]][q[1]]=rn(b-d,dtype)
        current=out
    return current

def ball_background(image,*,radius=1,height=1,dtype='float64'):
    h,w=shape(image);hh=frac(height)
    if radius<1 or hh<=0:raise DomainError('nonflat ball parameters')
    taps=[]
    for dy in range(-radius,radius+1):
        for dx in range(-radius,radius+1):
            d2=dy*dy+dx*dx
            if d2>radius*radius:continue
            if d2==0:b=0.0
            elif d2==radius*radius:b=rn(-hh)
            else:b=certify(lambda c:c.q(hh)*(c.q(1-Q(d2,radius*radius)).sqrt()-1))
            taps.append((dy,dx,frac(b)))
    erosion=zeros(h,w);out=zeros(h,w)
    for y in range(h):
        for x in range(w):erosion[y][x]=rn(min(sample(image,y+dy,x+dx,'reflect_half')-b for dy,dx,b in taps),dtype)
    for y in range(h):
        for x in range(w):out[y][x]=rn(max(sample(erosion,y-dy,x-dx,'reflect_half')+b for dy,dx,b in taps),dtype)
    return out

def subtract_background(image,background,*,restore_offset=0,dtype='float64'):
    h,w=same_shape(image,background);b=frac(restore_offset)
    return [[rn(frac(image[y][x])-frac(background[y][x])+b,dtype) for x in range(w)] for y in range(h)]

def flat_field(image,dark,flat,flat_dark,mask,*,reference_gain,invalid='error',dtype='float64'):
    h,w=same_shape(image,dark,flat,flat_dark,mask);g=frac(reference_gain)
    if g<=0 or invalid not in ('copy_input','zero','error'):raise DomainError('flat field parameters')
    out=zeros(h,w);valid=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            m=mask[y][x]
            if m not in (0,1):raise DomainError('binary valid_mask')
            den=frac(flat[y][x])-frac(flat_dark[y][x]) if m else Q()
            if m and den>0:
                out[y][x]=rn((frac(image[y][x])-frac(dark[y][x]))*g/den,dtype);valid[y][x]=1
            elif invalid=='copy_input':out[y][x]=image[y][x]
            elif invalid=='zero':out[y][x]=0.0
            else:raise DomainError('invalid flat calibration')
    return out,valid

def _rgb_shape(image):
    h,w=shape(image)
    if any(len(v)!=3 for row in image for v in row):raise DomainError('exactly three explicit RGB components required')
    return h,w

def airlight(image,*,radius=1,top_fraction=0.001,request_airlight=True):
    h,w=_rgb_shape(image);f=frac(top_fraction)
    if radius<0 or not 0<f<=1:raise DomainError('airlight parameters')
    rgb=[[[frac(v) for v in pixel] for pixel in row] for row in image]
    if any(v<0 for row in rgb for pixel in row for v in pixel):raise DomainError('nonnegative RGB required')
    dark=zeros(h,w)
    for y in range(h):
        for x in range(w):dark[y][x]=min(v for j,i in clipped_box(y,x,h,w,radius,radius) for v in rgb[j][i])
    if not request_airlight:return [[float(v) for v in row] for row in dark],None,None
    amount=max(1,-(-(f*h*w).numerator//(f*h*w).denominator))
    candidates=sorted([(y,x) for y in range(h) for x in range(w)],key=lambda p:(-dark[p[0]][p[1]],p))[:amount]
    p=min(candidates,key=lambda p:(-sum(rgb[p[0]][p[1]],Q()),p));a=rgb[p[0]][p[1]]
    if min(a)<=0:raise DomainError('nonpositive selected airlight')
    return [[float(v) for v in row] for row in dark],[rn(v) for v in a],p

def transmission(image,airlight,*,radius=1,omega=0.95,dtype='float64'):
    h,w=_rgb_shape(image);omega=frac(omega)
    if radius<0 or not 0<=omega<=1 or len(airlight)!=3:raise DomainError('transmission parameters')
    if omega==0:return zeros(h,w,1.0)
    a=list(map(frac,airlight))
    if min(a)<=0:raise DomainError('positive airlight required')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            ratios=[]
            for j,i in clipped_box(y,x,h,w,radius,radius):
                for c in range(3):
                    v=frac(image[j][i][c])
                    if v<0:raise DomainError('nonnegative RGB')
                    ratios.append(v/a[c])
            out[y][x]=rn(min(max(1-omega*min(ratios),0),1),dtype)
    return out

def apply_dehaze(image,airlight,transmission,*,t_floor=0.1,dtype='float64'):
    h,w=_rgb_shape(image);same_shape(image,transmission);floor=frac(t_floor)
    if not 0<floor<=1 or len(airlight)!=3:raise DomainError('dehaze parameters')
    out=[[None]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            t=frac(transmission[y][x])
            if not 0<=t<=1:raise DomainError('transmission range')
            te=max(t,floor);p=[]
            for c in range(3):
                v=frac(image[y][x][c])
                if v<0:raise DomainError('nonnegative input RGB')
                if t==1:p.append(image[y][x][c]);continue
                a=frac(airlight[c])
                if a<=0:raise DomainError('positive airlight')
                p.append(rn(a+(v-a)/te,dtype))
            out[y][x]=p
    return out
