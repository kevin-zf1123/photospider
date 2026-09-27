"""Small 2D-plane references for FIL. Batch/channel lifting is intentionally external.

Each public function evaluates the full requested small array; support_for_output
independently reports a point's exact source set. This is not a tile execution
engine, cancellation test, or runtime metadata/ownership implementation.
"""
from __future__ import annotations
from collections import deque
from .special import sum_products, quantile, negative
from .core import (Q,frac,rn,zeros,shape,same_shape,coordinate,sample,
                   DomainError,round_integer_even,solve_linear)

def convolution_geometry(h,w,kh,kw,anchor,output_shape,direction):
    ay,ax=anchor
    if not (0<=ay<kh and 0<=ax<kw):raise DomainError('anchor outside kernel')
    if direction not in ('convolve','correlate'):raise DomainError('direction')
    if output_shape=='same':return (h,w),(0,0)
    if output_shape=='full':
        return (h+kh-1,w+kw-1),((-ay,-ax) if direction=='convolve' else (ay-kh+1,ax-kw+1))
    if output_shape=='valid':
        if h<kh or w<kw:raise DomainError('nonpositive valid extent')
        return (h-kh+1,w-kw+1),((kh-1-ay,kw-1-ax) if direction=='convolve' else (ay,ax))
    raise DomainError('output shape')

def tap_position(y,x,j,i,anchor,direction):
    ay,ax=anchor
    return (y+ay-j,x+ax-i) if direction=='convolve' else (y+j-ay,x+i-ax)

def support_for_output(input_shape,kernel,anchor,q,direction='convolve',boundary='reflect_half',origin=(0,0)):
    h,w=input_shape;kh,kw=shape(kernel);out=set()
    for j in range(kh):
        for i in range(kw):
            if frac(kernel[j][i]):
                yy,xx=tap_position(q[0]+origin[0],q[1]+origin[1],j,i,anchor,direction)
                p=coordinate(yy,xx,h,w,boundary)
                if p is not None:out.add(p)
    return out

def convolve2d(image,kernel,anchor=(0,0),*,direction='convolve',normalization='none',bias=0,
               boundary='reflect_half',cval=0,output_shape='same',dtype='float64'):
    h,w=shape(image);kh,kw=shape(kernel)
    (oh,ow),origin=convolution_geometry(h,w,kh,kw,anchor,output_shape,direction)
    if boundary=='truncate':raise DomainError('ordinary convolution has no truncate')
    if output_shape!='same' and (boundary!='constant' or frac(cval)!=0):raise DomainError('full/valid require zero extension')
    k=[[frac(x) for x in row] for row in kernel]
    if normalization=='none':den=Q(1)
    elif normalization=='sum':den=sum((sum(row,Q()) for row in k),Q())
    elif normalization=='l1':den=sum((sum(map(abs,row),Q()) for row in k),Q())
    else:raise DomainError('normalization')
    if not den:raise DomainError('zero normalizer')
    bias_negative_zero = bias == 0 and negative(bias)
    bias=frac(bias);frac(cval);out=zeros(oh,ow)
    for y in range(oh):
        for x in range(ow):
            terms=[]
            for j in range(kh):
                for i in range(kw):
                    if k[j][i]:
                        yy,xx=tap_position(y+origin[0],x+origin[1],j,i,anchor,direction)
                        p=coordinate(yy,xx,h,w,boundary)
                        value=cval if p is None else image[p[0]][p[1]]
                        terms.append((k[j][i],value))
            total=sum_products(terms)
            # Special values bypass finite Fraction conversion; finite E remains exact.
            import math
            if isinstance(total,float) and not math.isfinite(total):
                out[y][x]=rn(total if math.isnan(total) else total/float(1 if den>0 else -1),dtype)
            else:
                exact=frac(total)/den+bias
                quotient_negative_zero = total == 0 and (negative(total) != (den < 0))
                out[y][x]=-0.0 if total == 0 and bias == 0 and quotient_negative_zero and bias_negative_zero else rn(exact,dtype)
    return out

def separable(image,kx,ky,anchor=(0,0),**kwargs):
    # This *exact* outer product deliberately is NOT a Float64 kernel tensor.
    kernel=[[frac(y)*frac(x) for x in kx] for y in ky]
    return convolve2d(image,kernel,anchor,**kwargs)

def normalized_convolution(image,mask,kernel,anchor=(0,0),*,boundary='truncate',cval=0,empty='copy_center',dtype='float64'):
    h,w=same_shape(image,mask);kh,kw=shape(kernel)
    convolution_geometry(h,w,kh,kw,anchor,'same','convolve')
    k=[[frac(x) for x in row] for row in kernel]
    if any(x<0 for row in k for x in row):raise DomainError('negative positive-kernel weight')
    if empty not in ('copy_center','zero','error'):raise DomainError('empty policy')
    out=zeros(h,w);valid=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            num=den=Q()
            for j in range(kh):
                for i in range(kw):
                    weight=k[j][i]
                    if not weight:continue
                    yy,xx=tap_position(y,x,j,i,anchor,'convolve')
                    p=coordinate(yy,xx,h,w,boundary)
                    if p is None:
                        if boundary=='truncate':continue
                        m=Q(1);v=frac(cval)
                    else:
                        m=frac(mask[p[0]][p[1]])
                        if not 0<=m<=1:raise DomainError('mask range')
                        if not m:continue
                        v=frac(image[p[0]][p[1]])
                    den+=weight*m;num+=weight*m*v
            if den:out[y][x]=rn(num/den,dtype);valid[y][x]=1
            elif empty=='copy_center':out[y][x]=image[y][x]
            elif empty=='zero':out[y][x]=0.0
            else:raise DomainError('zero normalized denominator')
    return out,valid

def window_samples(image,y,x,height,width,anchor,boundary,cval=0,footprint=None):
    values=[]
    for j in range(height):
        for i in range(width):
            if footprint is not None and not footprint[j][i]:continue
            yy,xx=y+j-anchor[0],x+i-anchor[1]
            h,w=shape(image);p=coordinate(yy,xx,h,w,boundary)
            if p is None:
                if boundary=='truncate':continue
                values.append(float(cval))
            else:values.append(image[p[0]][p[1]])
    return values

def box(image,height,width,anchor,*,mean=True,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image)
    if min(height,width)<1 or not(0<=anchor[0]<height and 0<=anchor[1]<width):raise DomainError('box geometry')
    if boundary=='truncate' and not mean:raise DomainError('sum does not support truncate')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a=window_samples(image,y,x,height,width,anchor,boundary,cval)
            out[y][x]=rn(sum(map(frac,a),Q())/(len(a) if mean else 1),dtype)
    return out

def percentile(image,footprint,anchor,*,q=Q(1,2),interpolation='linear',boundary='reflect_half',cval=0,empty='error',dtype='float64'):
    h,w=shape(image);kh,kw=shape(footprint);q=frac(q)
    if not 0<=q<=1 or not any(v for r in footprint for v in r):raise DomainError('q/footprint')
    if any(v not in (0,1) for r in footprint for v in r):raise DomainError('nonbinary footprint')
    if not(0<=anchor[0]<kh and 0<=anchor[1]<kw):raise DomainError('anchor')
    if interpolation not in ('linear','lower','higher','nearest_even','midpoint'):raise DomainError('interpolation')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a=window_samples(image,y,x,kh,kw,anchor,boundary,cval,footprint)
            if not a:
                if empty=='copy_center':out[y][x]=image[y][x];continue
                raise DomainError('empty footprint intersection')
            out[y][x]=quantile(a,q,interpolation,dtype)
    return out

def local_moments(image,height,width,anchor,*,other=None,ddof=0,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image)
    if other is not None:same_shape(image,other)
    if ddof<0:raise DomainError('ddof')
    means=zeros(h,w);moments=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a=list(map(frac,window_samples(image,y,x,height,width,anchor,boundary,cval)))
            b=a if other is None else list(map(frac,window_samples(other,y,x,height,width,anchor,boundary,cval)))
            n=len(a)
            if n<=ddof:raise DomainError('N <= ddof')
            sa,sb=sum(a,Q()),sum(b,Q())
            means[y][x]=rn(sa/n,dtype)
            moments[y][x]=rn((n*sum((u*v for u,v in zip(a,b)),Q())-sa*sb)/(n*(n-ddof)),dtype)
    return means,moments

def gradient(image,method='sobel',*,dx=1,dy=1,normalization='unit_ramp',boundary='reflect_half',cval=0,dtype='float64'):
    dx,dy=frac(dx),frac(dy)
    if min(dx,dy)<=0:raise DomainError('spacing')
    if method=='central':smooth=[0,1,0];den=2
    elif method=='sobel':smooth=[1,2,1];den=8
    elif method=='scharr':smooth=[3,10,3];den=32
    else:raise DomainError('gradient method')
    if normalization=='raw':den=1
    elif normalization!='unit_ramp':raise DomainError('gradient normalization')
    kx=[[Q(s*d,den)/dx for d in [-1,0,1]] for s in smooth]
    ky=[[Q(smooth[x]*[-1,0,1][y],den)/dy for x in range(3)] for y in range(3)]
    kw=dict(direction='correlate',boundary=boundary,cval=cval,dtype=dtype)
    return convolve2d(image,kx,(1,1),**kw),convolve2d(image,ky,(1,1),**kw)

def laplacian(image,*,eight=False,dx=1,dy=1,scale=1,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image);dx,dy,scale=map(frac,(dx,dy,scale))
    if min(dx,dy)<=0 or (eight and dx!=dy):raise DomainError('spacing')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            c=sample(image,y,x,boundary,cval)
            e,wv=sample(image,y,x+1,boundary,cval),sample(image,y,x-1,boundary,cval)
            n,s=sample(image,y-1,x,boundary,cval),sample(image,y+1,x,boundary,cval)
            if eight:
                corners=sum((sample(image,y+a,x+b,boundary,cval) for a in (-1,1) for b in (-1,1)),Q())
                v=(4*(e+wv+n+s)+corners-20*c)/(6*dx*dx)
            else:v=(e+wv-2*c)/(dx*dx)+(n+s-2*c)/(dy*dy)
            out[y][x]=rn(scale*v,dtype)
    return out

def hessian(image,*,dx=1,dy=1,boundary='reflect_half',cval=0,dtype='float64'):
    dx,dy=frac(dx),frac(dy)
    if min(dx,dy)<=0:raise DomainError('spacing')
    kernels=[[[0,0,0],[1/(dx*dx),-2/(dx*dx),1/(dx*dx)],[0,0,0]],
             [[1/(4*dx*dy),0,-1/(4*dx*dy)],[0,0,0],[-1/(4*dx*dy),0,1/(4*dx*dy)]],
             [[0,1/(dy*dy),0],[0,-2/(dy*dy),0],[0,1/(dy*dy),0]]]
    return tuple(convolve2d(image,k,(1,1),direction='correlate',boundary=boundary,cval=cval,dtype=dtype) for k in kernels)

def structure_tensor(gx,gy,height,width,anchor,*,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=same_shape(gx,gy);out=[zeros(h,w) for _ in range(3)]
    for y in range(h):
        for x in range(w):
            a=list(map(frac,window_samples(gx,y,x,height,width,anchor,boundary,cval)))
            b=list(map(frac,window_samples(gy,y,x,height,width,anchor,boundary,cval)))
            for z,terms in zip(out,[ [u*u for u in a],[u*v for u,v in zip(a,b)],[v*v for v in b] ]):
                z[y][x]=rn(sum(terms,Q())/len(a),dtype)
    return tuple(out)

def clipped_box(y,x,h,w,ry,rx):
    return [(j,i) for j in range(max(0,y-ry),min(h,y+ry+1)) for i in range(max(0,x-rx),min(w,x+rx+1))]

def guided(image,guide,radius_y=1,radius_x=1,*,epsilon=0.01,metric_scale=None,dtype='float64'):
    h,w=same_shape(image,guide);epsilon=frac(epsilon)
    if epsilon<=0 or min(radius_y,radius_x)<0:raise DomainError('guided parameters')
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    if not 1<=gdim<=4:raise DomainError('guide components')
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if len(scales)!=gdim or min(scales)<=0:raise DomainError('metric scale')
    g=[[[frac(guide[y][x][c] if vector else guide[y][x])*scales[c] for c in range(gdim)] for x in range(w)] for y in range(h)]
    aa=[[None]*w for _ in range(h)];bb=zeros(h,w)
    for y in range(h):
        for x in range(w):
            pts=clipped_box(y,x,h,w,radius_y,radius_x);n=len(pts)
            mg=[sum((g[j][i][c] for j,i in pts),Q())/n for c in range(gdim)]
            mp=sum((frac(image[j][i]) for j,i in pts),Q())/n
            cov=[[sum((g[j][i][a]*g[j][i][b] for j,i in pts),Q())/n-mg[a]*mg[b]+(epsilon if a==b else 0) for b in range(gdim)] for a in range(gdim)]
            cg=[sum((g[j][i][c]*frac(image[j][i]) for j,i in pts),Q())/n-mg[c]*mp for c in range(gdim)]
            a=[frac(rn(v)) for v in solve_linear(cov,cg)]
            aa[y][x]=a;bb[y][x]=frac(rn(mp-sum((u*v for u,v in zip(a,mg)),Q())))
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            pts=clipped_box(y,x,h,w,radius_y,radius_x);n=len(pts)
            ma=[sum((aa[j][i][c] for j,i in pts),Q())/n for c in range(gdim)]
            mb=sum((bb[j][i] for j,i in pts),Q())/n
            out[y][x]=rn(sum((a*b for a,b in zip(ma,g[y][x])),Q())+mb,dtype)
    return out

def variable_gather(image,radius,*,disk=False,max_radius=10,boundary='truncate',dtype='float64'):
    h,w=shape(image);mr=frac(max_radius)
    scalar=not isinstance(radius,(list,tuple))
    if not scalar:same_shape(image,radius)
    out=zeros(h,w);valid=zeros(h,w,1)
    for y in range(h):
        for x in range(w):
            r=frac(radius if scalar else radius[y][x])
            if not 0<=r<=mr:raise DomainError('radius out of static bound')
            if not r:out[y][x]=image[y][x];continue
            n=r.numerator//r.denominator;a=[]
            for dy in range(-n,n+1):
                for dx in range(-n,n+1):
                    if disk and dx*dx+dy*dy>r*r:continue
                    v=sample(image,y+dy,x+dx,boundary)
                    if v is not None:a.append(v)
            out[y][x]=rn(sum(a,Q())/len(a),dtype)
    return out,valid

def zero_dc_filter(image,kernel,*,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image);kh,kw=shape(kernel);ay,ax=kh//2,kw//2;out=zeros(h,w)
    k=[[frac(v) for v in row] for row in kernel]
    for y in range(h):
        for x in range(w):
            center=frac(image[y][x]);v=Q()
            for j in range(kh):
                for i in range(kw):
                    if (j,i)!=(ay,ax) and k[j][i]:v+=k[j][i]*(sample(image,y+j-ay,x+i-ax,boundary,cval)-center)
            out[y][x]=rn(v,dtype)
    return out

def hysteresis(weak,strong,connectivity=8):
    h,w=same_shape(weak,strong)
    if connectivity not in (4,8):raise DomainError('connectivity')
    if any(v not in (0,1) for a in (weak,strong) for r in a for v in r):raise DomainError('binary required')
    if any(strong[y][x] and not weak[y][x] for y in range(h) for x in range(w)):raise DomainError('strong not subset of weak')
    out=zeros(h,w,0);queue=deque()
    for y in range(h):
        for x in range(w):
            if strong[y][x]:out[y][x]=1;queue.append((y,x))
    neighbors=[(-1,0),(1,0),(0,-1),(0,1)]
    if connectivity==8:neighbors+=[(-1,-1),(-1,1),(1,-1),(1,1)]
    while queue:
        y,x=queue.popleft()
        for dy,dx in neighbors:
            yy,xx=y+dy,x+dx
            if 0<=yy<h and 0<=xx<w and weak[yy][xx] and not out[yy][xx]:
                out[yy][xx]=1;queue.append((yy,xx))
    return out

def canny(image,*,sigma=1,radius=1,low=0.1,high=0.2,norm='l2',connectivity=8,dtype='float64'):
    from .transcend import gaussian_kernel,magnitude
    low,high=frac(low),frac(high)
    if not 0<low<high:raise DomainError('canny thresholds')
    h,w=shape(image);k=gaussian_kernel(sigma,radius)
    b=separable(image,k,k,(radius,radius),normalization='sum',dtype=dtype)
    gx,gy=gradient(b,dtype=dtype);m=magnitude(gx,gy,norm=norm,dtype=dtype)
    weak=zeros(h,w,0);strong=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            a,bv=frac(gx[y][x]),frac(gy[y][x])
            if not a and not bv:continue
            if abs(a)>=2*abs(bv):dy,dx=0,1
            elif abs(bv)>=2*abs(a):dy,dx=1,0
            else:dy,dx=1,(1 if a*bv>=0 else -1)
            if not(0<=y+dy<h and 0<=x+dx<w and 0<=y-dy<h and 0<=x-dx<w):continue
            v=frac(m[y][x])
            if v>=frac(m[y+dy][x+dx]) and v>frac(m[y-dy][x-dx]):
                weak[y][x]=int(v>=low);strong[y][x]=int(v>=high)
    return hysteresis(weak,strong,connectivity)

def post_aa(image,guide,*,threshold=0.1,blend=Q(1,4),dtype='float64'):
    h,w=same_shape(image,guide);threshold,blend=frac(threshold),frac(blend)
    if threshold<=0 or not 0<=blend<=Q(1,2):raise DomainError('AA parameters')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            if not blend:out[y][x]=image[y][x];continue
            # Centre is part of the published guide validation support.
            sample(guide,y,x,'clamp')
            ex=abs(sample(guide,y,x+1,'clamp')-sample(guide,y,x-1,'clamp'))
            ey=abs(sample(guide,y+1,x,'clamp')-sample(guide,y-1,x,'clamp'))
            if max(ex,ey)<=threshold:out[y][x]=image[y][x];continue
            d=(1,0) if ex>=ey else (0,1)
            a=sample(image,y+d[0],x+d[1],'clamp');b=sample(image,y-d[0],x-d[1],'clamp')
            out[y][x]=rn((1-blend)*frac(image[y][x])+blend*(a+b)/2,dtype)
    return out

def straight_positive_blur(colors,alphas,weights,*,dtype='float64'):
    """Single output component of the proposed fused straight/alpha formula."""
    if not(len(colors)==len(alphas)==len(weights)):raise DomainError('tap lengths')
    total=alpha=premul=Q()
    for color,a,k in zip(colors,alphas,weights):
        k=frac(k)
        if k<0:raise DomainError('negative positive-kernel weight')
        if not k:continue
        a,c=frac(a),frac(color)
        if not 0<=a<=1:raise DomainError('alpha range')
        total+=k;alpha+=k*a;premul+=k*a*c
    if total<=0:raise DomainError('empty kernel')
    return (rn(premul/alpha,dtype) if alpha else 0.0),rn(alpha/total,dtype)
