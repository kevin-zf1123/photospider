"""Strict quantization and strictly rounded Float64 scan/screen references."""
from .precision import *
from .point import bounded

def _levels(levels):
    if not isinstance(levels,int) or not 2<=levels<=65536:raise DomainError('levels')
def _quantize(x,t,levels,edge,dtype):
    _levels(levels);u=bounded(x,edge);z=u*(levels-1);k=z.numerator//z.denominator;f=z-k
    return rn(F(min(levels-1,k+int(f>t)),levels-1),dtype)
def bayer_matrix(order=2):
    if not isinstance(order,int) or not 1<=order<=8:raise DomainError('Bayer order')
    b=[[0]]
    for _ in range(order):
        b=[[4*v for v in row]+[4*v+2 for v in row] for row in b]+[[4*v+3 for v in row]+[4*v+1 for v in row] for row in b]
    return b

def bayer(pixel,x,y,levels=2,order=2,origin=(0,0),phase=(0,0),edge='clamp',dtype=64):
    b=bayer_matrix(order);N=len(b);ox,oy=origin;px,py=phase
    if any(not isinstance(z,int) for z in (x,y,ox,oy,px,py)):raise DomainError('integer global coordinates required')
    t=F(2*b[(y-oy+py)%N][(x-ox+px)%N]+1,2*N*N)
    return _quantize(pixel,t,levels,edge,dtype)
def noise(pixel,noise,levels=2,edge='clamp',dtype=64):
    t=frac(noise)
    if not 0<=t<1:raise DomainError('noise not in [0,1)')
    return _quantize(pixel,t,levels,edge,dtype)
def ordered(pixel,x,y,thresholds,levels=2,origin=(0,0),phase=(0,0),edge='clamp',dtype=64):
    H=len(thresholds);W=len(thresholds[0]) if H else 0
    if not 1<=H<=4096 or not 1<=W<=4096 or any(len(r)!=W for r in thresholds):raise DomainError('threshold map shape')
    t=[[frac(v) for v in r] for r in thresholds]
    if any(not 0<v<1 for r in t for v in r):raise DomainError('threshold map values')
    ox,oy=origin;px,py=phase
    if any(not isinstance(z,int) for z in (x,y,ox,oy,px,py)):raise DomainError('integer coordinates')
    return _quantize(pixel,t[(y-oy+py)%H][(x-ox+px)%W],levels,edge,dtype)
def floyd_steinberg(image,levels=2,scan='serpentine',origin_y=0,edge='clamp',dtype=64):
    """Single-channel HxW mathematical reference; channels are independent."""
    _levels(levels);H=len(image);W=len(image[0]) if H else 0
    if not H or not W or any(len(r)!=W for r in image) or scan not in ('raster','serpentine') or not isinstance(origin_y,int):raise DomainError('FS image/scan')
    out=[[0.0]*W for _ in range(H)];current=[F(0)]*W
    for y in range(H):
        nxt=[F(0)]*W;direction=-1 if scan=='serpentine' and (y+origin_y)%2 else 1
        xs=range(W) if direction==1 else range(W-1,-1,-1)
        for x in xs:
            v=frac(rn(bounded(image[y][x],edge)+current[x],64))
            q=max(0,min(levels-1,rint(rn(v*(levels-1),64))))
            reconstruction=frac(rn(F(q,levels-1),64))
            out[y][x]=rn(reconstruction,dtype);error=frac(rn(v-reconstruction,64))
            if 0<=x+direction<W:current[x+direction]=frac(rn(current[x+direction]+frac(rn(error*F(7,16),64)),64))
            if y+1<H:
                for dx,w in ((-direction,F(3,16)),(0,F(5,16)),(direction,F(1,16))):
                    if 0<=x+dx<W:nxt[x+dx]=frac(rn(nxt[x+dx]+frac(rn(error*w,64)),64))
        current=nxt
    return out

def _screen(coverage,x,y,bx,by,origin,samples,edge,dtype,dot):
    c=bounded(coverage,edge);ax,ay,bx1,by1,ox,oy=map(frac,(*bx,*by,*origin));det=ax*by1-ay*bx1
    if det==0 or not isinstance(samples,int) or not 1<=samples<=16 or not isinstance(x,int) or not isinstance(y,int):raise DomainError('screen basis/samples/coordinates')
    if c==0 or c==1:return rn(c,dtype)
    count=0
    for j in range(samples):
        for i in range(samples):
            dx=F(x)+F(2*i+1,2*samples)-ox;dy=F(y)+F(2*j+1,2*samples)-oy
            u=((by1*dx-bx1*dy)/det)%1;v=((-ay*dx+ax*dy)/det)%1
            if not dot:inside=abs(u-F(1,2))<c/2
            else:
                d2=(u-F(1,2))**2+(v-F(1,2))**2
                if d2==0:rank=F(0)
                elif d2==F(1,2):rank=F(1)
                elif d2<=F(1,4):rank=pi()*d2
                else:rank=pi()*d2-4*d2*acos(1/(2*sqrt(d2)))+2*sqrt(d2-F(1,4))
                inside=compare(rank,c)<0
            count+=int(inside)
    return rn(F(count,samples*samples),dtype)
def halftone_dot(coverage,x,y,bx=(8,0),by=(0,8),origin=(0,0),samples=4,edge='reject',dtype=64):return _screen(coverage,x,y,bx,by,origin,samples,edge,dtype,True)
def halftone_line(coverage,x,y,bx=(8,0),by=(0,8),origin=(0,0),samples=4,edge='reject',dtype=64):return _screen(coverage,x,y,bx,by,origin,samples,edge,dtype,False)
