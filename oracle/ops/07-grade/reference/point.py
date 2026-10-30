"""Scalar/single-pixel reference functions, not a tensor/demand adapter."""
from .precision import *

def positive(x,allow_zero=False):
    x=frac(x)
    if x<0 or (x==0 and not allow_zero):raise DomainError('positive control required')
    return x

def exposure(x,ev,dtype=64):
    a,e=frac(x),frac(ev)
    if e==0 or a==0:return finite_copy(x,dtype)
    return rn(a*exp2(e),dtype)
def affine(x,gain=1,offset=0,dtype=64):
    a,g,o=map(frac,(x,gain,offset))
    return finite_copy(x,dtype) if (g,o)==(1,0) else rn(a*g+o,dtype)
def gain(x,g,dtype=64):return affine(x,positive(g),0,dtype)
def lgg(x,lift=0,gamma=1,gain=1,dtype=64):
    a,l=map(frac,(x,lift)); g=positive(gamma);k=positive(gain,True)
    return finite_copy(x,dtype) if (l,g,k)==(0,1,1) else rn(k*spow((1-l)*a+l,1/g),dtype)
def contrast_linear(x,contrast=1,pivot=.18,dtype=64):
    a,p=frac(x),frac(pivot);c=positive(contrast,True)
    return finite_copy(x,dtype) if c==1 else rn(p+(a-p)*c,dtype)
def contrast_log(x,contrast=1,pivot=.18,dtype=64):
    a=positive(x,True);c=positive(contrast);p=positive(pivot)
    return finite_copy(x,dtype) if c==1 or a==0 else rn(p*exp2(c*log2(a/p)),dtype)
def _levels(x,black,white,gamma,out0,out1,dtype,signed):
    a,b,w,o0,o1=map(frac,(x,black,white,out0,out1));g=positive(gamma)
    if not b<w:raise DomainError('black must be below white')
    u=(a-b)/(w-b)
    if not signed:u=clamp(u)
    if (b,w,g,o0,o1)==(0,1,1,0,1) and (signed or 0<=a<=1):return finite_copy(x,dtype)
    if u==0:return finite_copy(out0,dtype)
    if u==1:return finite_copy(out1,dtype)
    return rn(o0+(o1-o0)*(spow(u,1/g) if signed else power(u,1/g)),dtype)
def levels(x,black=0,white=1,gamma=1,out0=0,out1=1,dtype=64):return _levels(x,black,white,gamma,out0,out1,dtype,False)
def levels_signed(x,black=0,white=1,gamma=1,out0=0,out1=1,dtype=64):return _levels(x,black,white,gamma,out0,out1,dtype,True)
def _hmodel(v,dh,saturation_scale,third,dtype,hsv_mode):
    h,s,z=map(frac,v);dh=frac(dh);k=positive(saturation_scale,True);t=frac(third)
    if not(0<=s<=1 and 0<=z<=1):raise DomainError('bounded HSL/HSV coordinates required')
    if hsv_mode and t<0:raise DomainError('negative value scale')
    h1=finite_copy(v[0],dtype) if s==0 or dh==0 else rn(h+dh,dtype)
    s1=finite_copy(v[1],dtype) if k==1 else rn(clamp(s*k),dtype)
    z1=finite_copy(v[2],dtype) if t==(1 if hsv_mode else 0) else rn(clamp(z*t if hsv_mode else z+t),dtype)
    return [h1,s1,z1]
def hsl(v,delta_h=0,saturation_scale=1,delta_l=0,dtype=64):return _hmodel(v,delta_h,saturation_scale,delta_l,dtype,False)
def hsv(v,delta_h=0,saturation_scale=1,value_scale=1,dtype=64):return _hmodel(v,delta_h,saturation_scale,value_scale,dtype,True)
def lch(v,delta_l=0,chroma_scale=1,delta_h=0,dtype=64):
    l,c,h=map(frac,v);positive(c,True);k=positive(chroma_scale,True);dl,dh=map(frac,(delta_l,delta_h))
    return [affine(v[0],1,dl,dtype),affine(v[1],k,0,dtype),finite_copy(v[2],dtype) if c==0 else affine(v[2],1,dh,dtype)]
def monochrome(v,weights,bias=0,normalize=False,dtype=64):
    x,w=list(map(frac,v)),list(map(frac,weights));b=frac(bias)
    if len(x)!=3 or len(w)!=3:raise DomainError('RGB required')
    d=sum(w) if normalize else F(1)
    if d==0:raise DomainError('zero normalization sum')
    if b==0 and sum(z!=0 for z in w)==1 and max(w)==d:
        return finite_copy(v[next(i for i,z in enumerate(w) if z!=0)],dtype)
    return rn(sum(a*k for a,k in zip(x,w))/d+b,dtype)
def bounded(x,edge='clamp'):
    x=frac(x)
    if edge not in ('clamp','reject'):raise DomainError('edge mode')
    if edge=='reject' and not 0<=x<=1:raise DomainError('sample out of range')
    return clamp(x)
def posterize(x,levels=8,edge='clamp',dtype=64):
    if not isinstance(levels,int) or not 2<=levels<=65536:raise DomainError('levels')
    return rn(F(rint(bounded(x,edge)*(levels-1)),levels-1),dtype)
def threshold(x,t=.5):return int(frac(x)>=frac(t))
def mixer(v,matrix,bias,dtype=64):
    x=list(map(frac,v));b=list(map(frac,bias));M=[list(map(frac,r)) for r in matrix]
    if not 1<=len(x)<=64 or len(M)!=len(b) or not 1<=len(M)<=64 or any(len(r)!=len(x) for r in M):raise DomainError('matrix shape')
    out=[]
    for r,z in zip(M,b):
        ones=[i for i,k in enumerate(r) if k==1]
        if z==0 and len(ones)==1 and all(k in (0,1) for k in r):out.append(finite_copy(v[ones[0]],dtype))
        else:out.append(rn(sum(k*a for k,a in zip(r,x))+z,dtype))
    return out

def weights(w):
    w=list(map(frac,w))
    if len(w)!=3 or min(w)<0 or sum(w)<=0:raise DomainError('invalid zone weights')
    return [a/sum(w) for a in w]
def balance(v,zones,offsets,dtype=64):
    w=weights(zones);o=[list(map(frac,r)) for r in offsets]
    if len(o)!=3 or any(len(r)!=len(v) for r in o):raise DomainError('offset shape')
    return [affine(x,1,sum(w[j]*o[j][i] for j in range(3)),dtype) for i,x in enumerate(v)]
def balance_opponent(v,zones,offsets,dtype=64):return [finite_copy(v[0],dtype)]+balance(v[1:],zones,offsets,dtype)
def invert(x,lo=0,hi=1,dtype=64):
    x,a,b=map(frac,(x,lo,hi))
    if a>=b:raise DomainError('lo >= hi')
    return rn(a+b-x,dtype)
def zone_exact(f,edges=(0,.25,.75,1)):
    f=frac(f);s0,s1,h0,h1=map(frac,edges)
    if not s0<s1<=h0<h1:raise DomainError('invalid zone edges')
    a=smooth((f-s0)/(s1-s0));b=smooth((f-h0)/(h1-h0))
    return [1-a,a-b,b]
def zones(f,edges=(0,.25,.75,1),dtype=64):return [rn(z,dtype) for z in zone_exact(f,edges)]
def zone_grade(x,zones,controls,dtype=64):
    w=weights(zones);c=[list(map(frac,r)) for r in controls];a=frac(x)
    if len(c)!=3 or any(len(r)!=2 for r in c):raise DomainError('controls shape')
    e=sum(w[j]*c[j][0] for j in range(3));o=sum(w[j]*c[j][1] for j in range(3))
    return finite_copy(x,dtype) if e==0 and o==0 else rn(a*exp2(e)+o,dtype)
def local_grade(x,base,controls,edges,dtype=64):
    p,c,s,h=map(frac,controls);positive(c,True);b=frac(base);w=zone_exact(b,edges)
    e=(c-1)*(b-p)+s*w[0]+h*w[2]
    return exposure(x,e,dtype)
def hue_delta(h,chroma,delta,dtype=64):
    h0,c,d=map(frac,(h,chroma,delta));positive(c,True)
    return finite_copy(h,dtype) if c==0 or d==0 else rn(h0+d,dtype)
def chroma_scale(v,scale,dtype=64):
    k=positive(scale,True)
    return [finite_copy(v[0],dtype),affine(v[1],k,0,dtype),affine(v[2],k,0,dtype)]
def lightness_delta(v,delta,dtype=64):return [affine(v[0],1,delta,dtype),finite_copy(v[1],dtype),finite_copy(v[2],dtype)]
def reconstruct(v,source_y,target_y,mode,dtype=64):
    x=list(map(frac,v));s,t=map(frac,(source_y,target_y))
    if mode!='ratio':raise DomainError('ratio reconstruction required')
    if s==t:return [finite_copy(a,dtype) for a in v]
    return [rn(a*t/s,dtype) for a in x] if s else [rn(t,dtype)]*3
def saturation(v,scale,weights_y,dtype=64):
    x=list(map(frac,v));w=list(map(frac,weights_y));s=positive(scale,True)
    if len(x)!=3 or len(w)!=3 or sum(w)!=1:raise DomainError('exact normalized Y-row required')
    if s==1 or all(bits(v[0],dtype)==bits(a,dtype) for a in v):return [finite_copy(a,dtype) for a in v]
    y=sum(a*b for a,b in zip(x,w));return [rn(y+s*(a-y),dtype) for a in x]
def vibrance(v,strength=0,chroma_reference=.4,dtype=64):
    l,a,b=map(frac,v);s=frac(strength);ref=positive(chroma_reference)
    if not -1<=s<=1:raise DomainError('strength')
    if s==0 or a*a+b*b>=ref*ref:return [finite_copy(z,dtype) for z in v]
    q=sqrt(a*a+b*b)/ref;g=1+s*(1-q)*(1-q)
    return [finite_copy(v[0],dtype),rn(a*g,dtype),rn(b*g,dtype)]
def split_tone(v,zones,colors,strengths,dtype=64):
    st=[positive(x,True) for x in strengths]
    if len(st)!=3 or len(colors)!=3:raise DomainError('shape')
    return balance_opponent(v,zones,[[frac(t)*s for t in row] for row,s in zip(colors,st)],dtype)
def wheels(v,zones,controls,dtype=64):
    if len(controls)!=3 or any(len(row)!=3 for row in controls):raise DomainError('shape')
    return [zone_grade(v[c],zones,[controls[z][c] for z in range(3)],dtype) for c in range(3)]
def select_region(v,controls,dtype=64):
    l,c,h=map(frac,v);hc,hi,ho,c0,c1,c2,c3,l0,l1,l2,l3=map(frac,controls)
    if not (0<=hi<ho<=1 and 0<=c0<c1<=c2<c3 and l0<l1<=l2<l3 and c>=0):raise DomainError('region controls')
    d=abs((h-hc+1)%2-1)
    hw=1-smooth((d-hi)/(ho-hi));cw=smooth((c-c0)/(c1-c0))*(1-smooth((c-c2)/(c3-c2)));lw=smooth((l-l0)/(l1-l0))*(1-smooth((l-l2)/(l3-l2)))
    return rn(hw*cw*lw,dtype)
def selective(v,weights,delta,overlap='normalize',dtype=64):
    x=list(map(frac,v));w=list(map(frac,weights));d=[list(map(frac,r)) for r in delta]
    if not 1<=len(w)<=256 or len(d)!=len(w) or any(len(r)!=3 for r in d) or any(not 0<=z<=1 for z in w):raise DomainError('selection shape/domain')
    if overlap not in ('sum','normalize'):raise DomainError('overlap')
    den=1 if overlap=='sum' else max(1,sum(w))
    return [affine(v[c],1,sum(a*r[c] for a,r in zip(w,d))/den,dtype) for c in range(3)]

CDL_W=[F(2126,10000),F(7152,10000),F(722,10000)]
def _cdl(rgb,sop,saturation,dtype,rev,clipped):
    x=list(map(frac,rgb));sop=[list(map(frac,r)) for r in sop];sat=positive(saturation,not rev)
    if len(x)!=3 or len(sop)!=3 or any(len(r)!=3 for r in sop):raise DomainError('CDL shape')
    for s,o,p in sop:positive(s,not rev);positive(p)
    if not clipped and sat==1 and all(r==[1,0,1] for r in sop):return [finite_copy(v,dtype) for v in rgb]
    if rev:
        v=[clamp(a) for a in x] if clipped else x
        lum=sum(w*a for w,a in zip(CDL_W,v));u=[a if sat==1 else lum+(a-lum)/sat for a in v];out=[]
        for a,(s,o,p) in zip(u,sop):
            z=clamp(a) if clipped else a
            t=z if not clipped and compare(z)<0 else power(z,1/p)
            y=(t-o)/s;out.append(rn(clamp(y) if clipped else y,dtype))
        return out
    v=[]
    for a,(s,o,p) in zip(x,sop):
        t=a*s+o
        v.append(power(clamp(t),p) if clipped else t if t<0 else power(t,p))
    lum=sum(w*a for w,a in zip(CDL_W,v));out=[]
    for a in v:
        y=a if sat==1 else lum+sat*(a-lum);out.append(rn(clamp(y) if clipped else y,dtype))
    return out

def cdl_fwd(rgb,sop=((1,0,1),)*3,saturation=1,dtype=64):return _cdl(rgb,sop,saturation,dtype,False,True)
def cdl_rev(rgb,sop=((1,0,1),)*3,saturation=1,dtype=64):return _cdl(rgb,sop,saturation,dtype,True,True)
def cdl_fwd_no_clamp(rgb,sop=((1,0,1),)*3,saturation=1,dtype=64):return _cdl(rgb,sop,saturation,dtype,False,False)
def cdl_rev_no_clamp(rgb,sop=((1,0,1),)*3,saturation=1,dtype=64):return _cdl(rgb,sop,saturation,dtype,True,False)

def saturation_field(v,scale,dtype=64):
    """HSL/HSV [h,S,L/V]; only saturation is arithmetic."""
    s=frac(v[1]);k=positive(scale,True)
    if not 0<=s<=1:raise DomainError('bounded saturation required')
    y=finite_copy(v[1],dtype) if k==1 else rn(clamp(s*k),dtype)
    return [finite_copy(v[0],dtype),y,finite_copy(v[2],dtype)]
