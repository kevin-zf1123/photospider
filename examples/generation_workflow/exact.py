"""Independent finite rational / IEEE oracle. No Photospider runtime dependency.

Fractions retain the exact value of binary floating inputs. Algorithms labelled
exact return Fraction(s); round_bits/sqrt_fraction implement final IEEE RNE.
This is a test oracle, not a resource-accounted kernel implementation.
"""
from __future__ import annotations
import bisect
import math
import struct
from fractions import Fraction as F
from typing import Iterable, Sequence


def frac(x) -> F:
    if isinstance(x, float) and not math.isfinite(x):
        raise ValueError('finite input required')
    return F(x)


def power2(e: int) -> F:
    return F(1 << e) if e >= 0 else F(1, 1 << -e)


def floor_log2(x: F) -> int:
    x = frac(x)
    if x <= 0:
        raise ValueError('positive input required')
    e = x.numerator.bit_length() - x.denominator.bit_length()
    return e - (x < power2(e))


def _round_integer(n: int, d: int) -> int:
    q, r = divmod(n, d)
    return q + (2*r > d or (2*r == d and q & 1))


def _format(dtype: str):
    if dtype in ('float32', 'f32', 32):
        return 24, -126, 127, 8, 32
    if dtype in ('float64', 'f64', 64):
        return 53, -1022, 1023, 11, 64
    raise ValueError('dtype must be float32 or float64')


def round_bits(x, dtype='float64', negative_zero=False) -> int:
    """Correctly round any finite rational to IEEE bits; overflow raises.

    negative_zero applies only to a mathematically exact zero. A negative
    nonzero value that underflows produces -0 independently of this flag.
    """
    p, emin, emax, ebits, width = _format(dtype)
    x = frac(x)
    negative = x < 0 or (not x and negative_zero)
    sign = int(negative) << (width-1)
    x = abs(x)
    if not x:
        return sign
    e = floor_log2(x)
    unit = power2(max(e, emin) - (p-1))
    z = x / unit
    q = _round_integer(z.numerator, z.denominator)
    if e < emin:
        # q can reach exactly min-normal but cannot exceed it.
        return sign | q
    if q == 1 << p:
        q >>= 1
        e += 1
    if e > emax:
        raise OverflowError('finite destination overflow')
    exponent = e + ((1 << (ebits-1))-1)
    return sign | (exponent << (p-1)) | (q - (1 << (p-1)))


def from_bits(bits: int, dtype='float64') -> float:
    width = _format(dtype)[-1]
    if bits < 0 or bits >= 1 << width:
        raise ValueError('bits out of range')
    return struct.unpack('>f' if width == 32 else '>d', bits.to_bytes(width//8, 'big'))[0]


def float_bits(x: float, dtype='float64') -> int:
    width = _format(dtype)[-1]
    return int.from_bytes(struct.pack('>f' if width == 32 else '>d', x), 'big')


def round_fraction(x, dtype='float64', negative_zero=False) -> float:
    return from_bits(round_bits(x, dtype, negative_zero), dtype)


def bit_hex(x: float, dtype='float64') -> str:
    return f'{float_bits(x,dtype):0{_format(dtype)[-1]//4}x}'


def constant_bits(values: Sequence[int], height: int, width: int):
    if height < 1 or width < 1 or not values:
        raise ValueError('positive extents required')
    return [[list(values) for _ in range(width)] for _ in range(height)]


def sqrt_bounds(q, precision_bits: int = 160):
    """Certified rational enclosure of sqrt(q), via integer isqrt only."""
    q = frac(q)
    if q < 0 or precision_bits < 2:
        raise ValueError('invalid sqrt domain/precision')
    if not q:
        return F(0), F(0)
    e = floor_log2(q) // 2
    unit = power2(e - precision_bits)
    z = q / (unit*unit)
    a = math.isqrt(z.numerator // z.denominator)
    lo = a * unit
    return (lo, lo) if lo*lo == q else (lo, (a+1)*unit)


def sqrt_fraction(q, dtype='float64') -> float:
    """Correctly rounded sqrt(rational) using an exact midpoint comparison."""
    q = frac(q)
    if q < 0:
        raise ValueError('sqrt of negative rational')
    if not q:
        return 0.0
    p, emin, _, _, _ = _format(dtype)
    e = floor_log2(q)//2
    unit = power2(max(e, emin) - (p-1))
    z = q/(unit*unit)
    a = math.isqrt(z.numerator//z.denominator)
    cmp = 4*z.numerator - z.denominator*(2*a+1)**2
    a += cmp > 0 or (cmp == 0 and a & 1)
    return round_fraction(a*unit, dtype)


def round_sqrt_plus(q, b=0, dtype='float64') -> float:
    """Certified RN(sqrt(q)+b) for rational q,b, with explicit refinement cap."""
    q, b = frac(q), frac(b)
    if q < 0:
        raise ValueError('negative radicand')
    sn, sd = math.isqrt(q.numerator), math.isqrt(q.denominator)
    if sn*sn == q.numerator and sd*sd == q.denominator:
        return round_fraction(F(sn,sd)+b, dtype)
    for precision in (80,160,320,640,1280,2560,5120):
        lo, hi = sqrt_bounds(q, precision)
        a, z = round_bits(lo+b,dtype), round_bits(hi+b,dtype)
        if a == z:
            return from_bits(a,dtype)
    raise ArithmeticError('NotConverged: sqrt-plus rounding cell unresolved')


def accelerated_ok(reference: float, actual: float, dtype='float64') -> bool:
    """NUM final budget. Not a general accelerated admission proof."""
    width = _format(dtype)[-1]
    rbits, abits = float_bits(reference,dtype), float_bits(actual,dtype)
    if not math.isfinite(reference) or not math.isfinite(actual):
        return rbits == abits
    if reference == 0 or actual == 0:
        return rbits == abits
    if width == 32:
        def ordered(b):
            return ((~b) & 0xffffffff) if b & 0x80000000 else b | 0x80000000
        return abs(ordered(rbits)-ordered(abits)) <= 4
    r, a = F(reference), F(actual)
    if abs(r) < power2(-126) or abs(r) > F.from_float(float.fromhex('0x1.fffffep127')):
        return rbits == abits
    bound = 4*power2(floor_log2(abs(r))-23)
    return abs(a-r) <= bound


def coordinate(x, y, width, height, origin=(0,0), normalized=False):
    if not (width >= 1 and height >= 1 and 0 <= x < width and 0 <= y < height):
        raise ValueError('invalid canvas/index')
    if normalized:
        return F(2*x+1,2*width), F(2*y+1,2*height)
    return F(2*(origin[0]+x)+1,2), F(2*(origin[1]+y)+1,2)


def rectangle_coverage(bounds, pixel=(0,0)):
    x0,y0,x1,y1 = map(frac,bounds)
    if x1 < x0 or y1 < y0:
        raise ValueError('reversed bounds')
    x,y = map(frac,pixel)
    return max(F(0), min(x+1,x1)-max(x,x0))*max(F(0),min(y+1,y1)-max(y,y0))


def rectangle_sdf(p, bounds, dtype='float64'):
    x0,y0,x1,y1=map(frac,bounds)
    if x1 < x0 or y1 < y0:
        raise ValueError('reversed bounds')
    x,y=map(frac,p)
    qx=abs(x-(x0+x1)/2)-(x1-x0)/2
    qy=abs(y-(y0+y1)/2)-(y1-y0)/2
    if max(qx,qy) <= 0:
        return round_fraction(max(qx,qy),dtype)
    return sqrt_fraction(max(qx,0)**2+max(qy,0)**2,dtype)


def disk_sdf(p, center, radius, dtype='float64'):
    r=frac(radius)
    if r < 0:
        raise ValueError('negative radius')
    q=sum((frac(a)-frac(b))**2 for a,b in zip(p,center))
    return round_sqrt_plus(q,-r,dtype)


def rectangle_erosion(bounds, radius):
    a,b,c,d=map(frac,bounds);r=frac(radius)
    if r<0: raise ValueError('negative erosion radius')
    return None if c-a<=2*r or d-b<=2*r else (a+r,b+r,c-r,d-r)


def checker(p, cell, lo=0, hi=1, phase=(0,0)):
    cx,cy=map(frac,cell)
    if min(cx,cy)<=0: raise ValueError('positive cell size')
    ix=(frac(p[0])-frac(phase[0]))//cx
    iy=(frac(p[1])-frac(phase[1]))//cy
    return lo if (ix+iy)%2==0 else hi


def grid_pattern(p, cell, line_width, phase=(0,0), lo=0, hi=1):
    c=list(map(frac,cell));w=list(map(frac,line_width))
    if any(v<=0 for v in c) or any(not 0<=a<=b for a,b in zip(w,c)):
        raise ValueError('invalid grid')
    inside=any((frac(p[i])-frac(phase[i]))%c[i]<w[i] for i in range(2))
    return hi if inside else lo


def ramp(index, count, lo=0, hi=1):
    if count<1 or not 0<=index<count:raise ValueError('invalid ramp')
    return frac(lo) if count==1 else frac(lo)+(frac(hi)-frac(lo))*F(index,count-1)


def impulse(shape, index):
    if len(shape)!=len(index) or any(n<1 or i<0 or i>=n for n,i in zip(shape,index)):
        raise ValueError('invalid impulse')
    def at(query): return int(tuple(query)==tuple(index))
    return at


BARS=((1,1,1),(1,1,0),(0,1,1),(0,1,0),(1,0,1),(1,0,0),(0,0,1),(0,0,0))
def color_bar(x,width):
    if width<1 or not 0<=x<width:raise ValueError('invalid bar index')
    return BARS[min(7,(8*(2*x+1))//(2*width))]


def linear_coordinate(p,start,end):
    p,a,b=[tuple(map(frac,v)) for v in (p,start,end)]
    d=[b[i]-a[i] for i in range(2)];den=sum(v*v for v in d)
    if not den: raise ValueError('coincident gradient endpoints')
    return sum((p[i]-a[i])*d[i] for i in range(2))/den


def radial_coordinate(p,center,radii,dtype='float64'):
    r=list(map(frac,radii))
    if min(r)<=0:raise ValueError('positive radii')
    return sqrt_fraction(sum(((frac(p[i])-frac(center[i]))/r[i])**2 for i in range(2)),dtype)


def norm_coordinate(p,center,radii,metric):
    r=list(map(frac,radii))
    if min(r)<=0:raise ValueError('positive radii')
    q=[abs((frac(p[i])-frac(center[i]))/r[i]) for i in range(2)]
    if metric=='l1':return sum(q,F(0))
    if metric=='linf':return max(q)
    raise ValueError('metric')


def quadratic_coefficients(p,c0,r0,c1,r1):
    q=[frac(p[i])-frac(c0[i]) for i in range(2)]
    d=[frac(c1[i])-frac(c0[i]) for i in range(2)]
    r0,s=frac(r0),frac(r1)-frac(r0)
    return sum(a*a for a in d)-s*s,-2*(sum(a*b for a,b in zip(q,d))+r0*s),sum(a*a for a in q)-r0*r0


def spread(t,method):
    t=frac(t)
    if method=='pad':return min(max(t,F(0)),F(1))
    if method=='repeat':return t-t//1
    if method=='reflect':return 1-abs(1-t%2)
    raise ValueError('unknown spread')


def spread_rounded(t,method,dtype='float64'):
    if method=='pad' and isinstance(t,float) and t==0:
        return round_fraction(0,dtype,math.copysign(1,t)<0)
    q=spread(t,method);b=round_bits(q,dtype)
    return from_bits(b,dtype)


def numeric_lookup(t,table):
    t=frac(t)
    if len(table)<2 or not 0<=t<=1 or not table[0] or any(len(r)!=len(table[0]) for r in table):
        raise ValueError('invalid lookup')
    x=t*(len(table)-1);j=x//1
    if x==j:return list(map(frac,table[j]))
    w=x-j
    return [(1-w)*frac(a)+w*frac(b) for a,b in zip(table[j],table[j+1])]


def linear_rgba_mix(a,b,t):
    """Linear-light straight RGB subset; transparent black overrides copy."""
    t=frac(t)
    if len(a)!=4 or len(b)!=4 or not 0<=t<=1:raise ValueError('RGBA/t')
    aa,bb=list(map(frac,a)),list(map(frac,b))
    if not (0<=aa[3]<=1 and 0<=bb[3]<=1):raise ValueError('alpha range')
    if t==0 or t==1:
        selected=a if t==0 else b
        return list(selected) if frac(selected[3]) else [F(0)]*4
    alpha=(1-t)*aa[3]+t*bb[3]
    if not alpha:return [F(0)]*4
    # Identical-color selection preserves source zero signs as the CRV contract requires.
    if aa==bb and all((math.copysign(1,x) if isinstance(x,float) else 1)==
                      (math.copysign(1,y) if isinstance(y,float) else 1)
                      for x,y,q in zip(a,b,aa) if q==0):
        return list(a)
    rgb=[((1-t)*aa[3]*aa[i]+t*bb[3]*bb[i])/alpha for i in range(3)]
    return rgb+[alpha]


def bilinear(colors,u,v):
    u,v=frac(u),frac(v)
    if not (0<=u<=1 and 0<=v<=1):raise ValueError('uv')
    return [sum(w*frac(colors[j][i][c]) for j,i,w in
                ((0,0,(1-u)*(1-v)),(0,1,u*(1-v)),(1,0,(1-u)*v),(1,1,u*v)))
            for c in range(len(colors[0][0]))]


def barycentric(p,triangle):
    def cross(a,b):return a[0]*b[1]-a[1]*b[0]
    a,b,c=[tuple(map(frac,v)) for v in triangle];p=tuple(map(frac,p))
    v0=(b[0]-a[0],b[1]-a[1]);v1=(c[0]-a[0],c[1]-a[1]);v2=(p[0]-a[0],p[1]-a[1])
    den=cross(v0,v1)
    if not den:raise ValueError('degenerate triangle')
    v,w=cross(v2,v1)/den,cross(v0,v2)/den
    return 1-v-w,v,w


def bicubic_patch(colors,u,v):
    u,v=frac(u),frac(v)
    if not(0<=u<=1 and 0<=v<=1):raise ValueError('uv')
    bu=[math.comb(3,i)*u**i*(1-u)**(3-i) for i in range(4)]
    bv=[math.comb(3,j)*v**j*(1-v)**(3-j) for j in range(4)]
    return [sum(bu[i]*bv[j]*frac(colors[j][i][c]) for j in range(4) for i in range(4)) for c in range(len(colors[0][0]))]


def grid_points(bounds,nx,ny):
    x0,y0,x1,y1=map(frac,bounds)
    if nx<1 or ny<1 or x1<=x0 or y1<=y0:raise ValueError('grid bounds')
    out=[]
    for j in range(ny):
        for i in range(nx):
            left=x0+(x1-x0)*F(i,nx);right=x0+(x1-x0)*F(i+1,nx)
            top=y0+(y1-y0)*F(j,ny);bottom=y0+(y1-y0)*F(j+1,ny)
            x=round_fraction((left+right)/2);y=round_fraction((top+bottom)/2)
            # Half-open mapping belongs to the original cell, not the whole canvas.
            if F(x)>=right:x=math.nextafter(round_fraction(right),-math.inf)
            if F(y)>=bottom:y=math.nextafter(round_fraction(bottom),-math.inf)
            if not (left<=F(x)<right and top<=F(y)<bottom):raise ValueError('no representable cell point')
            out.append((x,y))
    if len(set(out))!=len(out):raise ValueError('duplicate rounded grid points')
    return out


def _boundary_index(i,n,mode):
    if 0<=i<n:return i
    if mode=='zero':return None
    if mode=='clamp':return max(0,min(n-1,i))
    if mode=='wrap':return i%n
    if mode=='reflect101':
        if n==1:return 0
        j=i%(2*(n-1));return j if j<n else 2*(n-1)-j
    raise ValueError('boundary')


def correlate_kernel(source,kernel,boundary='zero',normalization='none'):
    # Scalar HW fixture; channels are independent copies of this formula.
    h,w=len(source),len(source[0]);kh,kw=len(kernel),len(kernel[0])
    if kh%2!=1 or kw%2!=1:raise ValueError('odd kernel extents')
    if any(len(row)!=w for row in source) or any(len(row)!=kw for row in kernel):raise ValueError('ragged input')
    k=[list(map(frac,row)) for row in kernel]
    den=F(1) if normalization=='none' else sum(map(sum,k),F(0))
    if normalization not in ('none','sum') or not den:raise ValueError('normalization; l2 is separate measured fixture')
    result=[]
    for y in range(h):
        row=[]
        for x in range(w):
            total=F(0)
            for j in range(kh):
                yy=_boundary_index(y+j-kh//2,h,boundary)
                for i in range(kw):
                    xx=_boundary_index(x+i-kw//2,w,boundary)
                    if yy is not None and xx is not None:total+=k[j][i]*frac(source[yy][xx])
            row.append(total/den)
        result.append(row)
    return result


def width_linear(s,length,stops,widths,normalized=True):
    s,L=frac(s),frac(length);xs=list(map(frac,stops));ys=list(map(frac,widths))
    if len(xs)<2 or len(xs)!=len(ys) or any(a>=b for a,b in zip(xs,xs[1:])) or any(y<0 for y in ys):
        raise ValueError('width profile')
    if L<0 or not 0<=s<=L:raise ValueError('arc coordinate')
    if normalized and (xs[0]!=0 or xs[-1]!=1):raise ValueError('normalized stops')
    q=(s/L if L else F(0)) if normalized else s
    if not xs[0]<=q<=xs[-1]:raise ValueError('outside stops')
    i=bisect.bisect_left(xs,q)
    if xs[i]==q:return ys[i]
    t=(q-xs[i-1])/(xs[i]-xs[i-1]);return (1-t)*ys[i-1]+t*ys[i]


def pchip_fixture(xs,ys,q):
    # Independent Fraction implementation of the same published shape-preserving
    # formulas. This helper has no color/descriptor semantics.
    x,y=list(map(frac,xs)),list(map(frac,ys));q=frac(q)
    if len(x)<2 or len(x)!=len(y) or any(a>=b for a,b in zip(x,x[1:])) or not x[0]<=q<=x[-1]:raise ValueError('pchip domain')
    h=[b-a for a,b in zip(x,x[1:])];d=[(b-a)/z for a,b,z in zip(y,y[1:],h)]
    def endpoint(h0,h1,d0,d1):
        z=((2*h0+h1)*d0-h0*d1)/(h0+h1)
        if z*d0<=0:return F(0)
        return 3*d0 if d0*d1<0 and abs(z)>3*abs(d0) else z
    if len(x)==2:m=[d[0],d[0]]
    else:
        m=[endpoint(h[0],h[1],d[0],d[1])]
        for i in range(1,len(x)-1):
            if d[i-1]*d[i]<=0:m.append(F(0))
            else:
                w1,w2=2*h[i]+h[i-1],h[i]+2*h[i-1]
                m.append((w1+w2)/(w1/d[i-1]+w2/d[i]))
        m.append(endpoint(h[-1],h[-2],d[-1],d[-2]))
    j=bisect.bisect_left(x,q)
    if x[j]==q:return y[j]
    j-=1;t=(q-x[j])/h[j]
    return (2*t**3-3*t*t+1)*y[j]+(t**3-2*t*t+t)*h[j]*m[j]+(-2*t**3+3*t*t)*y[j+1]+(t**3-t*t)*h[j]*m[j+1]


def linear_grain(rgb,alpha,strength,noise):
    s=frac(strength)
    if s<0:raise ValueError('negative strength')
    if len(rgb)!=3:raise ValueError('three RGB components')
    n=noise if isinstance(noise,(list,tuple)) else [noise]*3
    if len(n)!=3:raise ValueError('three noise components')
    if s==0:return list(rgb),alpha
    return [frac(c)+s*frac(g) for c,g in zip(rgb,n)],alpha


# Stable descriptive aliases used by the draft specs (same helper semantics).
coordinates=coordinate
ramp_pattern=ramp
rgb_bars=color_bar
linear_gradient=linear_coordinate
radial_gradient=radial_coordinate
norm_gradient=norm_coordinate


def two_circle_rational(p,c0,r0,c1,r1,no_solution='valid_zero'):
    """Exact rational-root subset; irrational roots explicitly unsupported."""
    r0,r1=frac(r0),frac(r1)
    if no_solution not in ('valid_zero','reject'):raise ValueError('no_solution')
    if min(r0,r1)<0 or (tuple(map(frac,c0))==tuple(map(frac,c1)) and r0==r1):
        raise ValueError('invalid circles')
    A,B,C=quadratic_coefficients(p,c0,r0,c1,r1);s=r1-r0
    def absent():
        if no_solution=='reject':raise ValueError('NoSolution')
        return F(0),0
    if A==B==C==0:
        return (-r0/s,1) if s<0 else absent()
    if A==0:
        if B==0:return absent()
        roots=[-C/B]
    else:
        disc=B*B-4*A*C
        if disc<0:return absent()
        n,d=math.isqrt(disc.numerator),math.isqrt(disc.denominator)
        if n*n!=disc.numerator or d*d!=disc.denominator:
            raise NotImplementedError('irrational root certification is not implemented')
        root=F(n,d);roots=[(-B-root)/(2*A),(-B+root)/(2*A)]
    valid=[t for t in roots if r0+t*s>=0]
    return (max(valid),1) if valid else absent()


def source_width_coordinate(subpath_lengths,subpath,source_s,parameter_domain):
    """Exact source-position map, not a PathSet association/codec implementation."""
    lengths=list(map(frac,subpath_lengths));s=frac(source_s)
    if not lengths or min(lengths)<0 or not isinstance(subpath,int) or not 0<=subpath<len(lengths):
        raise ValueError('source subpath')
    if not 0<=s<=lengths[subpath]:raise ValueError('source arc coordinate')
    if parameter_domain=='subpath_arclength_px':return s
    if parameter_domain=='subpath_normalized_arclength':
        return s/lengths[subpath] if lengths[subpath] else F(0)
    if parameter_domain=='pathset_normalized_arclength':
        total=sum(lengths)
        return (sum(lengths[:subpath])+s)/total if total else F(0)
    raise ValueError('parameter domain')
