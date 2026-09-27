"""Directed-MPFR certified local transcendental references, with explicit caps."""
from .core import Q,frac,rn,zeros,shape,same_shape,sample,coordinate,DomainError
from .bounds import certify,exp_q,sqrt_q,atan2_q,Interval

def gaussian_kernel(sigma,radius):
    s=frac(sigma)
    if s<0 or not isinstance(radius,int) or radius<0 or (s==0 and radius!=0):raise DomainError('Gaussian sigma/radius')
    if radius>256:raise DomainError('oracle fixture cap radius=256; not a production spec limit')
    if not s:return [1.0]
    a={j:exp_q(-Q(j*j)/(2*s*s)) for j in range(radius+1)}
    return [a[abs(j)] for j in range(-radius,radius+1)]

def bilateral(image,guide=None,*,radius_y=1,radius_x=1,sigma_y=1,sigma_x=1,sigma_range=1,
              metric_scale=None,boundary='reflect_half',dtype='float64'):
    h,w=shape(image)
    if guide is None:guide=image
    same_shape(image,guide)
    sy,sx,sr=map(frac,(sigma_y,sigma_x,sigma_range))
    if min(sy,sx,sr)<=0 or min(radius_y,radius_x)<0:raise DomainError('bilateral parameters')
    if boundary not in ('reflect_half','reflect_whole','clamp','wrap','truncate'):raise DomainError('boundary')
    if radius_y==0 and radius_x==0:return [list(row) for row in image]
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    if not 1<=gdim<=4:raise DomainError('guide components')
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if len(scales)!=gdim or min(scales)<=0:raise DomainError('metric_scale')
    def g(y,x):return [frac(guide[y][x][c] if vector else guide[y][x])*scales[c] for c in range(gdim)]
    spatial={(dy,dx):frac(exp_q(-Q(dy*dy)/(2*sy*sy)-Q(dx*dx)/(2*sx*sx))) for dy in range(-radius_y,radius_y+1) for dx in range(-radius_x,radius_x+1)}
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            center=g(y,x);num=den=Q()
            for (dy,dx),ws in spatial.items():
                if not ws:continue
                p=coordinate(y+dy,x+dx,h,w,boundary)
                if p is None:continue
                other=g(*p);distance=sum(((a-b)**2 for a,b in zip(center,other)),Q())
                weight=ws*frac(exp_q(-distance/(2*sr*sr)))
                if weight:num+=weight*frac(image[p[0]][p[1]]);den+=weight
            out[y][x]=rn(num/den,dtype)
    return out

def magnitude(gx,gy,*,norm='l2',dtype='float64'):
    h,w=same_shape(gx,gy);out=zeros(h,w)
    if norm not in ('l1','l2'):raise DomainError('norm')
    for y in range(h):
        for x in range(w):
            a,b=frac(gx[y][x]),frac(gy[y][x])
            out[y][x]=sqrt_q(a*a+b*b,dtype) if norm=='l2' else rn(abs(a)+abs(b),dtype)
    return out

def orientation(gx,gy,*,unit='radian',dtype='float64'):
    h,w=same_shape(gx,gy);angle=zeros(h,w);valid=zeros(h,w,0)
    if unit not in ('radian','turn'):raise DomainError('unit')
    for y in range(h):
        for x in range(w):
            a,b=frac(gx[y][x]),frac(gy[y][x]);valid[y][x]=int(bool(a or b))
            angle[y][x]=atan2_q(b,a,dtype,turn=(unit=='turn'))
    return angle,valid

def _atan_interval(c,y,x):
    if not y:
        return c.pi() if x<0 else c.q(0)
    yy,xx=c.q(y),c.q(x)
    lo=[c.binary('atan2',a,b,c.DOWN) for a in (yy.lo,yy.hi) for b in (xx.lo,xx.hi)]
    hi=[c.binary('atan2',a,b,c.UP) for a in (yy.lo,yy.hi) for b in (xx.lo,xx.hi)]
    return Interval(c,c.lo(lo),c.hi(hi))

def eigen2d(a,b,c,*,dtype='float64'):
    h,w=same_shape(a,b,c);large=zeros(h,w);small=zeros(h,w);angle=zeros(h,w);valid=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            aa,bb,cc=map(frac,(a[y][x],b[y][x],c[y][x]));d=(aa-cc)**2+4*bb*bb
            if not bb:
                large[y][x]=rn(max(aa,cc),dtype);small[y][x]=rn(min(aa,cc),dtype)
            else:
                large[y][x]=certify(lambda ctx:(aa+cc+ctx.q(d).sqrt())/2,dtype)
                # A rational zero determinant gives an exactly zero eigenvalue.
                if aa*cc==bb*bb and aa+cc>=0:small[y][x]=0.0
                else:small[y][x]=certify(lambda ctx:(aa+cc-ctx.q(d).sqrt())/2,dtype)
            if aa!=cc or bb:
                valid[y][x]=1
                angle[y][x]=certify(lambda ctx:_atan_interval(ctx,2*bb,aa-cc)/2+(ctx.pi() if bb<0 else 0),dtype)
    return large,small,angle,valid

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
