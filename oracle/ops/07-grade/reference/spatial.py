"""Small-image exact spatial references; intentionally not optimized."""
from .precision import *

def neighborhood(image,x,y,radius):
    H=len(image);W=len(image[0]) if H else 0
    if not isinstance(radius,int) or not 0<=radius<=4096 or not (0<=x<W and 0<=y<H) or any(len(r)!=W for r in image):raise DomainError('image/radius/coordinate')
    return [(i,j,image[j][i]) for j in range(max(0,y-radius),min(H,y+radius+1)) for i in range(max(0,x-radius),min(W,x+radius+1))]
def threshold_mean(image,x,y,radius=3,offset=0):
    n=neighborhood(image,x,y,radius);m=sum(frac(v) for _,_,v in n)/len(n)
    return int(frac(image[y][x])>=m-frac(offset))

def threshold_gaussian(image,x,y,radius=3,offset=0,sigma=2):
    sigma=frac(sigma)
    if sigma<=0:raise DomainError('sigma')
    neighborhood_values=neighborhood(image,x,y,radius)
    if all(frac(v)==frac(image[y][x]) for _,_,v in neighborhood_values):
        return int(frac(offset)>=0)
    # Compare after multiplying by the positive denominator. Combining equal
    # spatial weights proves exact symmetric ties without an epsilon.
    coefficients={}
    for i,j,v in neighborhood_values:
        distance=(i-x)**2+(j-y)**2
        coefficients[distance]=coefficients.get(distance,F(0))+frac(image[y][x])+frac(offset)-frac(v)
    difference=sum((exp(-F(distance)/(2*sigma*sigma))*coefficient for distance,coefficient in coefficients.items()),F(0))
    return int(compare(difference)>=0)

def threshold_sauvola(image,x,y,radius=7,k=.2,dynamic_range=.5):
    k,r=map(frac,(k,dynamic_range))
    n=[frac(v) for _,_,v in neighborhood(image,x,y,radius)]
    if not 0<=k<=1 or r<=0 or min(n)<0 or max(n)>1:raise DomainError('Sauvola domain')
    mean=sum(n)/len(n);variance=sum((v-mean)**2 for v in n)/len(n)
    threshold=mean*(1+k*(sqrt(variance)/r-1))
    return int(compare(frac(image[y][x]),threshold)>=0)
def bilateral_base(image,x,y,radius=3,sigma_space=2,sigma_range=1,epsilon=2**-16):
    n=neighborhood(image,x,y,radius);ss,sr,ep=map(frac,(sigma_space,sigma_range,epsilon))
    if min(ss,sr,ep)<=0:raise DomainError('bilateral controls')
    raw=[frac(v) for _,_,v in n]
    if min(raw)<0:raise DomainError('negative luminance')
    clamped=[max(v,ep) for v in raw];center=max(frac(image[y][x]),ep)
    z=log2(center)
    if all(v==center for v in clamped):return rn(z,64)
    num=F(0);den=F(0)
    for (i,j,_),v in zip(n,clamped):
        zi=log2(v);dz=z-zi
        w=F(1) if i==x and j==y else exp(-F((i-x)**2+(j-y)**2)/(2*ss*ss)-dz*dz/(2*sr*sr))
        num+=w*zi;den+=w
    return rn(num/den,64)
def vignette(x,y,center=(0,0),radii=(1,1),rotation_pi=0,inner=0,outer=1,center_ev=0,edge_ev=-1):
    x,y,cx,cy,rx,ry,angle,i,o,e0,e1=map(frac,(x,y,*center,*radii,rotation_pi,inner,outer,center_ev,edge_ev))
    if rx<=0 or ry<=0 or i<0 or o<=i:raise DomainError('vignette geometry')
    if e0==e1:return finite_copy(center_ev,64)
    co=trigpi(angle,True);si=trigpi(angle);dx=x-cx;dy=y-cy
    u=(co*dx+si*dy)/rx;v=(-si*dx+co*dy)/ry
    r2=(dx*dx+dy*dy)/(rx*rx) if rx==ry else u*u+v*v
    if compare(r2,i*i)<=0:return finite_copy(center_ev,64)
    if compare(r2,o*o)>=0:return finite_copy(edge_ev,64)
    return rn(e0+(e1-e0)*smooth((sqrt(r2)-i)/(o-i)),64)
