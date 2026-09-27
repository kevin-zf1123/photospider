"""Exact algebraic reduction + MPFR enclosure for tiny DFT/DCT fixtures.

Roots of unity are reduced modulo a cyclotomic polynomial over Q. This proves
exact spectral cancellations (including DC/constant and odd lengths) instead of
letting an interval containing true zero refine forever. Numerical evaluation
then encloses the reduced algebraic expression. Explicit caps are oracle-only.
"""
from __future__ import annotations
from functools import lru_cache
import math
from .core import Q,frac,rn,zeros,shape,same_shape,DomainError
from .bounds import certify,sqrt_q

@lru_cache(None)
def cyclotomic(n):
    if not 1<=n<=2048:raise DomainError('oracle cyclotomic order cap 2048')
    p=[-1]+[0]*(n-1)+[1]
    for d in range(1,n):
        if n%d:continue
        divisor=list(cyclotomic(d));quot=[0]*(len(p)-len(divisor)+1);rem=p[:]
        for i in range(len(quot)-1,-1,-1):
            a=rem[i+len(divisor)-1];quot[i]=a
            for j,b in enumerate(divisor):rem[i+j]-=a*b
        if any(rem):raise AssertionError('nonexact cyclotomic division')
        p=quot
    return tuple(p)

def reduce_poly(p,n):
    phi=cyclotomic(n);a=list(p)
    while a and not a[-1]:a.pop()
    for i in range(len(a)-len(phi),-1,-1):
        v=a[i+len(phi)-1]
        if v:
            for j,q in enumerate(phi):a[i+j]-=v*q
    a=a[:len(phi)-1]
    while a and not a[-1]:a.pop()
    return a

def evaluate_real_poly(p,n,*,scale=Q(1),sqrt_scale=Q(1),dtype='float64'):
    p=reduce_poly(p,n);scale=frac(scale);sqrt_scale=frac(sqrt_scale)
    if not p or not scale:return 0.0
    if len(p)==1:
        a,b=math.isqrt(sqrt_scale.numerator),math.isqrt(sqrt_scale.denominator)
        if a*a==sqrt_scale.numerator and b*b==sqrt_scale.denominator:return rn(p[0]*scale*Q(a,b),dtype)
    return certify(lambda c:sum((c.cospi(Q(2*j,n))*q for j,q in enumerate(p) if q),c.q(0))*scale*c.q(sqrt_scale).sqrt(),dtype)

def dft2(real,imag=None,*,inverse=False,norm='backward',dtype='float64'):
    h,w=shape(real)
    if h*w>64:raise DomainError('DFT oracle cap is 64 pixels, not a runtime limit')
    if imag is None:imag=zeros(h,w)
    same_shape(real,imag)
    if norm not in ('backward','ortho'):raise DomainError('DFT norm')
    rr=[[frac(v) for v in row] for row in real];ii=[[frac(v) for v in row] for row in imag]
    n=math.lcm(h,w,4);pr=zeros(h,w);pi=zeros(h,w)
    scale=Q(1,h*w) if inverse and norm=='backward' else Q(1)
    sq=Q(1,h*w) if norm=='ortho' else Q(1)
    sign=1 if inverse else -1
    for ky in range(h):
        for kx in range(w):
            poly=[Q() for _ in range(n)]
            for y in range(h):
                for x in range(w):
                    e=sign*(ky*y*(n//h)+kx*x*(n//w))%n
                    poly[e]+=rr[y][x];poly[(e+n//4)%n]+=ii[y][x]
            re=[Q() for _ in range(n)];im=[Q() for _ in range(n)]
            for e,a in enumerate(poly):
                re[e]+=a/2;re[-e%n]+=a/2
                im[(e-n//4)%n]+=a/2;im[(-e-n//4)%n]-=a/2
            pr[ky][kx]=evaluate_real_poly(re,n,scale=scale,sqrt_scale=sq,dtype=dtype)
            pi[ky][kx]=evaluate_real_poly(im,n,scale=scale,sqrt_scale=sq,dtype=dtype)
    return pr,pi

def rfft2(image,*,norm='backward',dtype='float64'):
    h,w=shape(image);real,imag=dft2(image,norm=norm,dtype=dtype)
    return [r[:w//2+1] for r in real],[r[:w//2+1] for r in imag]

def validate_half(real,imag,original_width):
    h,k=same_shape(real,imag);w=original_width
    if not isinstance(w,int) or w<=0 or k!=w//2+1:raise DomainError('original_width mismatch')
    columns=[0]+([w//2] if w%2==0 else [])
    for y in range(h):
        for x in range(k):frac(real[y][x]);frac(imag[y][x])
    for x in columns:
        for y in range(h):
            yy=(-y)%h
            if frac(real[y][x])!=frac(real[yy][x]) or frac(imag[y][x])!=-frac(imag[yy][x]):
                raise DomainError('non-Hermitian boundary column')
    return h,w

def irfft2(real,imag,original_width,*,norm='backward',dtype='float64'):
    h,w=validate_half(real,imag,original_width);k=w//2+1
    rr=zeros(h,w);ii=zeros(h,w)
    for y in range(h):
        for x in range(w):
            if x<k:rr[y][x]=real[y][x];ii[y][x]=imag[y][x]
            else:rr[y][x]=real[-y%h][w-x];ii[y][x]=-imag[-y%h][w-x]
    return dft2(rr,ii,inverse=True,norm=norm,dtype=dtype)[0]

def project_hermitian(real,imag,*,dtype='float64'):
    h,w=same_shape(real,imag);rr=zeros(h,w);ii=zeros(h,w);seen=set()
    for y in range(h):
        for x in range(w):
            if (y,x) in seen:continue
            yy,xx=-y%h,-x%w;seen|={(y,x),(yy,xx)}
            a,b,c,d=map(frac,(real[y][x],imag[y][x],real[yy][xx],imag[yy][xx]))
            if (y,x)==(yy,xx):
                rr[y][x]=real[y][x];ii[y][x]=0.0;continue
            re=rn((a+c)/2,dtype);im=rn((b-d)/2,dtype)
            rr[y][x]=rr[yy][xx]=re
            ii[y][x]=0.0 if im==0 else im;ii[yy][xx]=0.0 if im==0 else -im
    return rr,ii

def shift(image,*,inverse=False):
    h,w=shape(image);sy=h//2 if inverse else (h+1)//2;sx=w//2 if inverse else (w+1)//2
    return [[image[(y+sy)%h][(x+sx)%w] for x in range(w)] for y in range(h)]

def _window1(n,kind,sampling,alpha):
    if n<1:raise DomainError('window length')
    if n==1:return [1.0]
    den=n-1 if sampling=='symmetric' else n
    out=[]
    for i in range(n):
        t=Q(i,den)
        if kind=='rectangular' or (kind=='tukey' and alpha==0):v=1.0
        elif kind=='hann' or (kind=='tukey' and alpha==1):
            v=certify(lambda c:(1-c.cospi(2*t))/2)
        elif t<alpha/2:v=certify(lambda c:(1+c.cospi(2*t/alpha-1))/2)
        elif t<=1-alpha/2:v=1.0
        else:v=certify(lambda c:(1+c.cospi(2*t/alpha-2/alpha+1))/2)
        out.append(v)
    return out

def window(height,width,*,kind='hann',sampling='symmetric',alpha=None):
    if kind not in ('rectangular','hann','tukey') or sampling not in ('symmetric','periodic'):raise DomainError('window profile')
    if kind!='tukey' and alpha is not None:raise DomainError('unexpected alpha')
    a=Q(1,2) if alpha is None else frac(alpha)
    if not 0<=a<=1:raise DomainError('Tukey alpha')
    wy,wx=_window1(height,kind,sampling,a),_window1(width,kind,sampling,a)
    return [[rn(frac(y)*frac(x)) for x in wx] for y in wy]

def apply_window(image,win,*,dtype='float64'):
    h,w=same_shape(image,win)
    return [[rn(frac(image[y][x])*frac(win[y][x]),dtype) for x in range(w)] for y in range(h)]

def window_stats(win):
    h,w=shape(win);a=[frac(v) for row in win for v in row];n=h*w;s=sum(a,Q());s2=sum((v*v for v in a),Q())
    if not s:raise DomainError('zero coherent sum')
    return rn(s/n),rn(s2/n),rn(n*s2/(s*s))

def frequency_grid(height,width,dy=1,dx=1):
    dy,dx=frac(dy),frac(dx)
    if min(height,width)<1 or min(dy,dx)<=0:raise DomainError('frequency grid')
    return [[(Q(y if y<=height//2 else y-height)/(height*dy),Q(x if x<=width//2 else x-width)/(width*dx)) for x in range(width)] for y in range(height)]

def _low(c,r2,cutoff,kind,order):
    if kind=='ideal':return c.q(int(r2<=cutoff*cutoff))
    if kind=='gaussian':return c.q(-r2/(2*cutoff*cutoff)).exp()
    return 1/c.q(1+(r2/(cutoff*cutoff))**order).sqrt()

def radial_response(height,width,*,cutoff,kind='gaussian',high=False,order=1,dy=1,dx=1):
    fc=frac(cutoff)
    if fc<=0 or kind not in ('ideal','gaussian','butterworth') or not isinstance(order,int) or order<1:raise DomainError('radial response')
    grid=frequency_grid(height,width,dy,dx);out=zeros(height,width);cache={}
    for y in range(height):
        for x in range(width):
            fy,fx=grid[y][x];r2=fy*fy+fx*fx
            if r2 not in cache:
                cache[r2]=certify(lambda c:1-_low(c,r2,fc,kind,order) if high else _low(c,r2,fc,kind,order))
            out[y][x]=cache[r2]
    return out,zeros(height,width)

def band_response(height,width,*,cutoff_low,cutoff_high,kind='gaussian',reject=False,order=1,dy=1,dx=1):
    lo,hi=frac(cutoff_low),frac(cutoff_high)
    if not 0<lo<hi or kind not in ('ideal','gaussian','butterworth') or order<1:raise DomainError('band response')
    grid=frequency_grid(height,width,dy,dx);out=zeros(height,width)
    for y in range(height):
        for x in range(width):
            fy,fx=grid[y][x];r2=fy*fy+fx*fx
            if r2==0:out[y][x]=float(bool(reject));continue
            out[y][x]=certify(lambda c:1-(_low(c,r2,hi,kind,order)-_low(c,r2,lo,kind,order)) if reject else _low(c,r2,hi,kind,order)-_low(c,r2,lo,kind,order))
    return out,zeros(height,width)

def torus_difference(d,period):
    d=frac(d)%period
    return d-period if d>period/2 else d

def notch_response(height,width,*,centers,sigma_y,sigma_x,depth=1,dy=1,dx=1):
    sy,sx,dep,dy,dx=map(frac,(sigma_y,sigma_x,depth,dy,dx))
    if min(sy,sx,dy,dx)<=0 or not 0<=dep<=1 or not centers:raise DomainError('notch parameters')
    py,px=1/dy,1/dx;cs=[];seen=set()
    for cy,cx in centers:
        c=(torus_difference(frac(cy),py),torus_difference(frac(cx),px))
        inv=(torus_difference(-c[0],py),torus_difference(-c[1],px))
        if c in seen or inv in seen:raise DomainError('duplicate notch conjugacy orbit')
        seen|={c,inv};cs.append(c)
        if inv!=c:cs.append(inv)
    grid=frequency_grid(height,width,dy,dx);out=zeros(height,width)
    for y in range(height):
        for x in range(width):
            fy,fx=grid[y][x]
            ds=[-((torus_difference(fy-cy,py)/sy)**2+(torus_difference(fx-cx,px)/sx)**2)/2 for cy,cx in cs]
            if dep==0:out[y][x]=1.0;continue
            if dep==1 and any(v==0 for v in ds):out[y][x]=0.0;continue
            def expr(c):
                v=c.q(1)
                for d in ds:v=v*(1-dep*c.q(d).exp())
                return v
            out[y][x]=certify(expr)
    return out,zeros(height,width)

def multiply(fr,fi,hr,hi,*,dtype='float64'):
    h,w=same_shape(fr,fi,hr,hi);rr=zeros(h,w);ii=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a,b,c,d=map(frac,(fr[y][x],fi[y][x],hr[y][x],hi[y][x]))
            rr[y][x]=rn(a*c-b*d,dtype);ii[y][x]=rn(a*d+b*c,dtype)
    return rr,ii

def block_partition(height,width,payload_height,payload_width,kernel_height,kernel_width,*,save=False):
    if min(height,width,payload_height,payload_width,kernel_height,kernel_width)<1:raise DomainError('block geometry')
    blocks=[]
    for y in range(0,height,payload_height):
        for x in range(0,width,payload_width):
            hh,ww=min(payload_height,height-y),min(payload_width,width-x)
            blocks.append({'payload':(y,x,hh,ww),'logical_read':(y-kernel_height+1,x-kernel_width+1,hh+kernel_height-1,ww+kernel_width-1) if save else (y,x,hh,ww),'discard_prefix':(kernel_height-1,kernel_width-1) if save else (0,0)})
    return blocks

def _dct_entry(n,k,j,kind,norm,inverse):
    if norm=='ortho':
        if inverse:k,j=j,k
        if kind=='I':return Q(k*j,n-1),Q(1),int(k in (0,n-1))+int(j in (0,n-1))
        if kind=='II':return Q(k*(2*j+1),2*n),Q(1),int(k==0)
        if kind=='III':return Q(j*(2*k+1),2*n),Q(1),int(j==0)
        return Q((2*k+1)*(2*j+1),4*n),Q(1),0
    t={'I':'I','II':'III','III':'II','IV':'IV'}[kind] if inverse else kind
    if t=='I':return Q(k*j,n-1),Q(1 if j in (0,n-1) else 2),0
    if t=='II':return Q(k*(2*j+1),2*n),Q(2),0
    if t=='III':return Q(j*(2*k+1),2*n),Q(1 if j==0 else 2),0
    return Q((2*k+1)*(2*j+1),4*n),Q(2),0

def dct2(image,*,kind='II',norm='backward',inverse=False,dtype='float64'):
    h,w=shape(image)
    if h*w>36:raise DomainError('DCT oracle cap 36 pixels')
    if kind not in ('I','II','III','IV') or norm not in ('backward','ortho'):raise DomainError('DCT profile')
    if kind=='I' and min(h,w)<2:raise DomainError('DCT-I length')
    image=[[frac(v) for v in row] for row in image];out=zeros(h,w)
    sqrt_scale=Q(4,(h-1)*(w-1) if kind=='I' else h*w) if norm=='ortho' else Q(1)
    scale=Q(1,4*(h-1)*(w-1) if kind=='I' else 4*h*w) if inverse and norm=='backward' else Q(1)
    for ky in range(h):
        for kx in range(w):
            terms=[]
            for y in range(h):
                for x in range(w):
                    ph1,a,s1=_dct_entry(h,ky,y,kind,norm,inverse);ph2,b,s2=_dct_entry(w,kx,x,kind,norm,inverse)
                    s=s1+s2;v=image[y][x]*a*b/(2*(2**(s//2)))
                    terms.extend([(ph1+ph2,v,s%2),(ph1-ph2,v,s%2)])
            n=math.lcm(8,*[2*p.denominator for p,_,_ in terms]);poly=[Q() for _ in range(n)]
            if n>2048:raise DomainError('DCT root order cap')
            for phase,v,odd in terms:
                e=int(phase*n/2)%n
                if odd:
                    for a in (e,-e):
                        for b in (n//8,-n//8):poly[(a+b)%n]+=v/4
                else:
                    poly[e]+=v/2;poly[-e%n]+=v/2
            out[ky][kx]=evaluate_real_poly(poly,n,scale=scale,sqrt_scale=sqrt_scale,dtype=dtype)
    return out
