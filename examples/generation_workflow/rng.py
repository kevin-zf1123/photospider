"""Independent Philox integer oracle and exact finite noise models.

Philox4x64-10 is checked against external Random123 KATs (see SOURCES.md).
Address packing and bit extraction below are CANDIDATE layout experiment-1,
not a frozen Photospider sequence. All addressed stochastic helpers inherit this limit.
No upstream implementation is bundled. Bridson's transcendental candidates below
are explicitly MEASURED, not correctly-rounded strict golden sequences.
"""
from __future__ import annotations
import math
from fractions import Fraction as F
from exact import frac, round_fraction, sqrt_fraction

MASK=(1<<64)-1
MASK32=(1<<32)-1
CANDIDATE_LAYOUT="philox4x64-layout-experiment-1"
DOMAINS={'uniform':1,'gaussian':2,'jitter':3,'rejection':4,'bridson':5,
         'gradient':6,'cellular':7,'poisson':8,'speckle':9,'grain':10,'resource':11}


def _integer(v,lo,hi,name):
    if not isinstance(v,int) or isinstance(v,bool) or not lo<=v<=hi:
        raise ValueError(f'{name} outside [{lo},{hi}]')
    return v


def philox4x64(counter,key,rounds=10):
    if rounds!=10:raise ValueError('this contract freezes exactly ten rounds')
    if len(counter)!=4 or len(key)!=2:raise ValueError('counter/key width')
    c=[_integer(v,0,MASK,'counter word') for v in counter]
    k=[_integer(v,0,MASK,'key word') for v in key]
    for r in range(10):
        p0=0xD2E7470EE14C6C93*c[0];p1=0xCA5A826395121157*c[2]
        c=[((p1>>64)^c[1]^k[0])&MASK,p1&MASK,((p0>>64)^c[3]^k[1])&MASK,p0&MASK]
        if r!=9:k=[(k[0]+0x9E3779B97F4A7C15)&MASK,(k[1]+0xBB67AE8584CAA73B)&MASK]
    return tuple(c)


def pack_counter(x,y,channel=0,domain=1,stream=0,frame=0,draw=0):
    _integer(x,-(1<<31),(1<<31)-1,'x');_integer(y,-(1<<31),(1<<31)-1,'y')
    _integer(channel,0,65535,'channel');_integer(domain,0,255,'domain')
    _integer(stream,0,MASK32,'stream');_integer(frame,0,MASK32,'frame');_integer(draw,0,MASK32,'draw')
    return (x&MASK32)|((y&MASK32)<<32),frame|(draw<<32),stream|(channel<<32)|(domain<<48),0


def block(x,y,channel=0,domain=1,seed=0,stream=0,frame=0,draw=0):
    _integer(seed,-(1<<63),(1<<63)-1,'seed')
    s=seed&((1<<64)-1)
    return philox4x64(pack_counter(x,y,channel,domain,stream,frame,draw),(s,0))


def halfopen53(a,b):return F(((a>>37)<<26)|(b>>38),1<<53)
def open52(a,b):return F(2*(((a>>38)<<26)|(b>>38))+1,1<<53)

def uniform(x,y,channel=0,dtype='float64',**kw):
    b=block(x,y,channel,domain=1,**kw)
    if dtype=='float32':return float(F(b[0]>>40,1<<24))
    if dtype=='float64':return float(halfopen53(b[0],b[1]))
    raise ValueError('dtype')


def gaussian_inputs(x,y,channel=0,domain=2,**kw):
    if domain not in (2,10):raise ValueError('Gaussian domain')
    b=block(x,y,channel,domain=domain,**kw)
    return open52(b[0],b[1]),halfopen53(b[2],b[3])


def poisson_input(x,y,channel=0,**kw):
    b=block(x,y,channel,domain=8,**kw);return open52(b[0],b[1])


def _inside_map(u,lo,hi):
    lo,hi,u=frac(lo),frac(hi),frac(u)
    if not lo<hi or not 0<=u<1:raise ValueError('half-open interval')
    x=round_fraction(lo+(hi-lo)*u)
    if F(x)>=hi:
        x=math.nextafter(round_fraction(hi),-math.inf)
    if not lo<=F(x)<hi:raise ValueError('no representable interior value')
    return x


def _bounds(bounds):
    if len(bounds)!=4:raise ValueError('bounds width')
    x0,y0,x1,y1=map(frac,bounds)
    if x0>=x1 or y0>=y1:raise ValueError('positive area bounds')
    return x0,y0,x1,y1


def jitter_points(bounds,nx,ny,jitter=1,seed=0,stream=0):
    x0,y0,x1,y1=_bounds(bounds);jitter=frac(jitter)
    if nx<1 or ny<1 or not 0<=jitter<=1:raise ValueError('grid/jitter')
    out=[]
    for j in range(ny):
        for i in range(nx):
            b=block(i,j,domain=3,seed=seed,stream=stream)
            u,v=halfopen53(b[0],b[1]),halfopen53(b[2],b[3])
            left=x0+F(i,nx)*(x1-x0);right=x0+F(i+1,nx)*(x1-x0)
            top=y0+F(j,ny)*(y1-y0);bottom=y0+F(j+1,ny)*(y1-y0)
            out.append((_inside_map(F(1,2)+jitter*(u-F(1,2)),left,right),
                        _inside_map(F(1,2)+jitter*(v-F(1,2)),top,bottom)))
    if len(set(out))!=len(out):raise ValueError('rounded duplicate point')
    return out


def distance2(a,b):return sum((frac(x)-frac(y))**2 for x,y in zip(a,b))


def pairwise_distances(points,min_distance):
    r=frac(min_distance)
    if r<=0:raise ValueError('positive distance')
    return all(distance2(a,b)>=r*r for i,a in enumerate(points) for b in points[:i])


def rejection_points(bounds,min_distance,candidate_count,seed=0,stream=0,max_count=1048576,with_status=False):
    x0,y0,x1,y1=_bounds(bounds);r=frac(min_distance)
    _integer(candidate_count,0,(1<<31)-1,'candidate_count')
    _integer(max_count,1,(1<<63)-1,'max_count')
    if r<=0:raise ValueError('positive distance')
    points=[]
    for i in range(candidate_count):
        b=block(i,0,domain=4,seed=seed,stream=stream)
        p=(_inside_map(halfopen53(b[0],b[1]),x0,x1),_inside_map(halfopen53(b[2],b[3]),y0,y1))
        if all(distance2(p,q)>=r*r for q in points):
            points.append(p)
            if len(points)==max_count:
                return {'points':points,'stop_reason':'count_reached'} if with_status else points
    return {'points':points,'stop_reason':'candidates_exhausted'} if with_status else points


def bridson_reference(bounds,min_distance,k=30,seed=0,stream=0,max_count=1048576,max_iterations=2097153,with_status=False):
    """MEASURED float candidates + exact acceptance; NOT a strict sequence oracle."""
    x0,y0,x1,y1=_bounds(bounds);r=frac(min_distance)
    if r<=0 or not 1<=k<=256 or max_count<1:raise ValueError('Bridson parameters')
    _integer(max_count,1,(1<<63)-1,"max_count")
    _integer(max_iterations,0,(1<<31)-1,"max_iterations")
    b=block(0,0,domain=5,seed=seed,stream=stream)
    points=[(_inside_map(halfopen53(b[0],b[1]),x0,x1),_inside_map(halfopen53(b[2],b[3]),y0,y1))]
    active=[0];iteration=0
    while active:
        if len(points)==max_count:
            return {'points':points,'stop_reason':'count_reached'} if with_status else points
        if iteration>=max_iterations:raise ArithmeticError('NotConverged: iteration limit')
        parent=points[active[0]];accepted=False
        for attempt in range(k):
            b=block(iteration+1,attempt,domain=5,seed=seed,stream=stream)
            u,v=float(halfopen53(b[0],b[1])),float(halfopen53(b[2],b[3]))
            rho=float(r)*math.sqrt(1+3*u);theta=2*math.pi*v
            p=(parent[0]+rho*math.cos(theta),parent[1]+rho*math.sin(theta))
            if not all(math.isfinite(v) for v in p):raise OverflowError('candidate overflow')
            if not(x0<=F(p[0])<x1 and y0<=F(p[1])<y1):continue
            if any(distance2(p,q)<r*r for q in points):continue
            points.append(p);active.append(len(points)-1);accepted=True;break
        if not accepted:active.pop(0)
        iteration+=1
    return {'points':points,'stop_reason':'active_exhausted'} if with_status else points


# Fixed numerical permutation from the author reference (S08). This is data,
# not copied Java implementation; weighted polynomial arithmetic below is new.
PERM_2002=(
151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,
77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,
135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,
5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,
49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180)


def fade(t):t=frac(t);return t*t*t*(t*(6*t-15)+10)


def _perlin_gradient(h,x,y,z):
    h&=15;u=x if h<8 else y
    v=y if h<4 else (x if h in (12,14) else z)
    return (-u if h&1 else u)+(-v if h&2 else v)


def perlin2002_fraction(coordinates):
    if len(coordinates)!=3:raise ValueError('3D coordinates')
    q=list(map(frac,coordinates));cell=[v//1 for v in q];f=[v-i for v,i in zip(q,cell)]
    weights=[(1-fade(t),fade(t)) for t in f]
    def p(i):return PERM_2002[i%256]
    total=F(0)
    for dx in (0,1):
        for dy in (0,1):
            for dz in (0,1):
                h=p(p(p(cell[0]+dx)+cell[1]+dy)+cell[2]+dz)
                g=_perlin_gradient(h,f[0]-dx,f[1]-dy,f[2]-dz)
                total+=weights[0][dx]*weights[1][dy]*weights[2][dz]*g
    return total


GRADIENTS_2D=((1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,1),(1,-1),(-1,-1))
def gradient2d_fraction(coordinates,seed=0,stream=0,frame=0,period=(0,0)):
    if len(coordinates)!=2 or len(period)!=2:raise ValueError('2D coordinate/period')
    q=list(map(frac,coordinates));cell=[v//1 for v in q];f=[v-i for v,i in zip(q,cell)]
    for v in period:_integer(v,0,(1<<31)-1,'period')
    weights=[(1-fade(t),fade(t)) for t in f];total=F(0)
    for dx in (0,1):
        for dy in (0,1):
            ix,iy=cell[0]+dx,cell[1]+dy
            _integer(ix,-(1<<31),(1<<31)-1,'lattice x');_integer(iy,-(1<<31),(1<<31)-1,'lattice y')
            if period[0]:ix%=period[0]
            if period[1]:iy%=period[1]
            b=block(ix,iy,domain=6,seed=seed,stream=stream,frame=frame)
            g=GRADIENTS_2D[b[0]&7]
            dot=g[0]*(f[0]-dx)+g[1]*(f[1]-dy)
            total+=weights[0][dx]*weights[1][dy]*dot
    return total


def cellular_feature(ix,iy,**kw):
    b=block(ix,iy,domain=7,**kw)
    return _inside_map(halfopen53(b[0],b[1]),ix,ix+1),_inside_map(halfopen53(b[2],b[3]),iy,iy+1)


def cellular2d(p,max_search_rings=64,dtype='float64',**kw):
    p=tuple(map(frac,p));ix,iy=(v//1 for v in p)
    candidates=[]
    for ring in range(max_search_rings+1):
        for y in range(iy-ring,iy+ring+1):
            for x in range(ix-ring,ix+ring+1):
                if max(abs(x-ix),abs(y-iy))!=ring:continue
                point=cellular_feature(x,y,**kw)
                candidates.append((distance2(p,point),y,x,point))
        candidates.sort(key=lambda q:q[:3])
        if len(candidates)>=2:
            lower=min(p[0]-(ix-ring),(ix+ring+1)-p[0],p[1]-(iy-ring),(iy+ring+1)-p[1])
            if lower*lower>candidates[1][0]:
                a,b=candidates[:2]
                return {'f1':sqrt_fraction(a[0],dtype),'f2':sqrt_fraction(b[0],dtype),
                        'cell':(a[2],a[1]),'distance2':(a[0],b[0]),
                        'rings':ring,'unvisited_lower2':lower*lower}
    raise ArithmeticError('NotConverged: F2 search certificate absent')


def fractal_fraction(coordinates,kind='fbm',basis='gradient2d_philox',octaves=4,
                     frequency=1,lacunarity=2,gain=F(1,2),normalization='amplitude_sum',
                     footprint=None,**kw):
    q=tuple(map(frac,coordinates));freq,lac,gain=map(frac,(frequency,lacunarity,gain))
    if not 1<=octaves<=32 or freq<=0 or lac<=1 or not 0<=gain<=1:raise ValueError('fractal parameters')
    if kind not in ('fbm','turbulence','ridged'):raise ValueError('fractal kind')
    if basis not in ('gradient2d_philox','perlin2002_3d'):raise ValueError('basis')
    if normalization not in ('none','amplitude_sum'):raise ValueError('normalization')
    footprint=None if footprint is None else frac(footprint)
    if footprint is not None and (footprint<0 or kind!='fbm'):raise ValueError('footprint requires fbm')
    total=F(0);den=F(0);amp=F(1)
    for i in range(octaves):
        den+=amp
        weight=amp*(max(F(0),min(F(1),2-2*footprint*freq)) if footprint is not None else 1)
        if weight:
            at=[v*freq for v in q]
            n=gradient2d_fraction(at,**kw) if basis=='gradient2d_philox' else perlin2002_fraction(at)
            term=n if kind=='fbm' else (abs(n) if kind=='turbulence' else (1-abs(n)/2)**2)
            total+=weight*term
        freq*=lac;amp*=gain
    return total/(den if normalization=='amplitude_sum' else 1)


def advected_fraction(p,velocity,time,**kw):
    t=frac(time)
    if kw.get('frame',0)!=0:raise ValueError('continuous base frame must be zero')
    return gradient2d_fraction([frac(a)-frac(b)*t for a,b in zip(p,velocity)],**kw)


def temporal_fraction(p,time,time_scale=1,z_offset=0):
    scale=frac(time_scale)
    if scale<=0:raise ValueError('positive time_scale')
    return perlin2002_fraction([frac(p[0]),frac(p[1]),frac(time)*scale+frac(z_offset)])
