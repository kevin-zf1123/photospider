"""Independent exact finite 2D geometry oracle (Fraction arithmetic).

This module does not implement PathSet ABI, ObjectId associations, budgets or
CPU backend admission. Polygon area/arrangement routines are intentionally slow
reference algorithms for finite fixtures, including self-crossing and overlap.
Curve transcendental/root/inverse-length cases are not silently approximated.
"""
from __future__ import annotations
from fractions import Fraction as F
from functools import cmp_to_key
import math
from exact import frac, round_fraction, round_bits, sqrt_fraction, sqrt_bounds


def point(p):
    if len(p)!=2:raise ValueError('2D point')
    return tuple(map(frac,p))

def add(a,b):return tuple(x+y for x,y in zip(a,b))
def sub(a,b):return tuple(x-y for x,y in zip(a,b))
def mul(a,s):return tuple(x*s for x in a)
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def cross(a,b):return a[0]*b[1]-a[1]*b[0]
def norm2(a):return dot(a,a)

def orientation(a,b,c):
    a,b,c=map(point,(a,b,c));return cross(sub(b,a),sub(c,a))


def point_segment_distance2(p,a,b):
    p,a,b=map(point,(p,a,b));v=sub(b,a);den=norm2(v)
    t=min(F(1),max(F(0),dot(sub(p,a),v)/den)) if den else F(0)
    closest=add(a,mul(v,t))
    return norm2(sub(p,closest)),t,closest


def pairwise_distances(points,min_distance):
    r=frac(min_distance)
    if r<=0:raise ValueError('positive distance')
    pp=list(map(point,points))
    return all(norm2(sub(a,b))>=r*r for i,a in enumerate(pp) for b in pp[:i])


def nearest_points(p,points,ids=None,dtype='float64'):
    ids=list(range(len(points))) if ids is None else list(ids)
    if len(points)<2:raise ValueError('NoSolution: at least two points required')
    if len(ids)!=len(points) or len(set(ids))!=len(ids):raise ValueError('unique ID map')
    p=point(p)
    ds=sorted((norm2(sub(p,point(v))),i) for v,i in zip(points,ids))
    return sqrt_fraction(ds[0][0],dtype),sqrt_fraction(ds[1][0],dtype),ds[0][1]


def bezier_bernstein(controls,t):
    p=list(map(point,controls));t=frac(t);d=len(p)-1
    if d not in (1,2,3) or not 0<=t<=1:raise ValueError('Bezier degree/t')
    return tuple(sum(F(math.comb(d,i))*(1-t)**(d-i)*t**i*p[i][c] for i in range(d+1)) for c in range(2))


def bezier_casteljau(controls,t):
    work=list(map(point,controls));t=frac(t)
    if not work or not 0<=t<=1:raise ValueError('Bezier/t')
    while len(work)>1:work=[add(mul(a,1-t),mul(b,t)) for a,b in zip(work,work[1:])]
    return work[0]


def bezier_derivative(controls,t):
    p=list(map(point,controls));d=len(p)-1
    if d<1:raise ValueError('degree')
    return bezier_casteljau([mul(sub(b,a),d) for a,b in zip(p,p[1:])],t)


def rounded_bezier_position(controls,t,dtype='float64'):
    t=frac(t)
    if t in (0,1):
        p=controls[0 if t==0 else -1]
        return tuple(round_fraction(frac(v),dtype, isinstance(v,float) and math.copysign(1,v)<0 and v==0) for v in p)
    q=bezier_bernstein(controls,t)
    return tuple(round_fraction(v,dtype,all(isinstance(p[c],float) and p[c]==0 and math.copysign(1,p[c])<0 for p in controls)) for c,v in enumerate(q))


def unit_tangent_fixture(derivative,dtype='float64'):
    d=point(derivative);q=norm2(d)
    if not q:return (0.0,0.0),0
    # d/sqrt(q) = sign(d)*sqrt(d^2/q); exact midpoint sqrt routine.
    return tuple(math.copysign(sqrt_fraction(v*v/q,dtype),float(v)) if v else 0.0 for v in d),1


def split_bezier(controls,t=F(1,2)):
    work=list(map(point,controls));t=frac(t)
    if not 0<=t<=1:raise ValueError('split parameter')
    left=[work[0]];right=[work[-1]]
    while len(work)>1:
        work=[add(mul(a,1-t),mul(b,t)) for a,b in zip(work,work[1:])]
        left.append(work[0]);right.append(work[-1])
    return left,list(reversed(right))


def trim_bezier_t(controls,a,b):
    a,b=frac(a),frac(b)
    if not 0<=a<=b<=1:raise ValueError('trim interval')
    if a==b:return []
    if a==0 and b==1:return list(map(point,controls))
    left,_=split_bezier(controls,b)
    return split_bezier(left,a/b)[1] if a else left


def reverse_bezier(controls):return list(reversed(controls))


def hermite(data,t,derivative=False):
    if len(data)!=4:raise ValueError('P0,P1,d0,d1')
    p0,p1,d0,d1=map(point,data);t=frac(t)
    if not 0<=t<=1:raise ValueError('t')
    h=(6*t*t-6*t,-6*t*t+6*t,3*t*t-4*t+1,3*t*t-2*t) if derivative else (2*t**3-3*t*t+1,-2*t**3+3*t*t,t**3-2*t*t+t,t**3-t*t)
    return tuple(sum(a*v[c] for a,v in zip(h,(p0,p1,d0,d1))) for c in range(2))


def bspline_fraction(controls,knots,degree,t,weights=None,derivative=False):
    p=list(map(point,controls));k=list(map(frac,knots));t=frac(t);n=len(p)
    if not 0<=degree<=16 or n<degree+1 or len(k)!=n+degree+1 or any(a>b for a,b in zip(k,k[1:])) or not 0<=t<=1:
        raise ValueError('spline dimensions/domain')
    a,b=k[degree],k[n]
    if a>=b:raise ValueError('empty knot interval')
    w=[F(1)]*n if weights is None else list(map(frac,weights))
    if len(w)!=n or min(w)<=0:raise ValueError('positive weights')
    u=a+t*(b-a);left_limit=t==1
    cache={}
    def basis(i,d):
        if i<0 or i+d+1>=len(k):return F(0)
        key=(i,d)
        if key in cache:return cache[key]
        if d==0:
            ans=F(int(k[i]<u<=k[i+1] if left_limit else k[i]<=u<k[i+1]))
        else:
            den0,den1=k[i+d]-k[i],k[i+d+1]-k[i+1]
            ans=((u-k[i])*basis(i,d-1)/den0 if den0 else 0)+((k[i+d+1]-u)*basis(i+1,d-1)/den1 if den1 else 0)
        cache[key]=ans;return ans
    nb=[basis(i,degree) for i in range(n)]
    den=sum(z*v for z,v in zip(nb,w));num=[sum(z*v*c[j] for z,v,c in zip(nb,w,p)) for j in range(2)]
    if not den:raise ValueError('zero rational denominator')
    if not derivative:return tuple(x/den for x in num)
    db=[]
    for i in range(n):
        if degree==0:db.append(F(0));continue
        d0,d1=k[i+degree]-k[i],k[i+degree+1]-k[i+1]
        db.append((degree*basis(i,degree-1)/d0 if d0 else 0)-(degree*basis(i+1,degree-1)/d1 if d1 else 0))
    dd=sum(z*v for z,v in zip(db,w));dn=[sum(z*v*c[j] for z,v,c in zip(db,w,p)) for j in range(2)]
    return tuple((dn[j]*den-num[j]*dd)/(den*den)*(b-a) for j in range(2))


def _sqrt_sum_le(a,b,e):
    """Exact test sqrt(a)+sqrt(b)<=e, for nonnegative rationals."""
    if e<0:return False
    z=e*e-a-b
    return z>=0 and z*z>=4*a*b


def flatten_bezier(controls,epsilon=F(1,20),max_depth=24,max_segments=1048576):
    p=list(map(point,controls));d=len(p)-1;epsilon=frac(epsilon)
    if d not in (1,2,3) or epsilon<=0:raise ValueError('flatten input')
    leaves=[]
    def visit(cp,t0,t1,depth):
        target=[add(mul(cp[0],F(d-i,d)),mul(cp[-1],F(i,d))) for i in range(d+1)]
        deviation2=max(norm2(sub(a,b)) for a,b in zip(cp,target))
        published=[point(tuple(round_fraction(v) for v in cp[i])) for i in (0,-1)]
        rounding2=max(norm2(sub(a,b)) for a,b in zip((cp[0],cp[-1]),published))
        if _sqrt_sum_le(deviation2,rounding2,epsilon):
            if len(leaves)>=max_segments:raise OverflowError('CapacityLimit')
            leaves.append({'t0':t0,'t1':t1,'controls':cp,'a':published[0],'b':published[1],
                           'deviation2':deviation2,'rounding2':rounding2,'epsilon':epsilon})
        else:
            if depth>=max_depth:raise ArithmeticError('NotConverged: flatten depth')
            left,right=split_bezier(cp);mid=(t0+t1)/2
            visit(left,t0,mid,depth+1);visit(right,mid,t1,depth+1)
    visit(p,F(0),F(1),0)
    return [leaves[0]['a']]+[x['b'] for x in leaves],leaves


def verify_flatten_certificate(leaves):
    if not leaves:return False
    if leaves[0]['t0']!=0 or leaves[-1]['t1']!=1:return False
    for i,leaf in enumerate(leaves):
        cp=leaf['controls'];d=len(cp)-1
        dev=max(norm2(sub(cp[j],add(mul(cp[0],F(d-j,d)),mul(cp[-1],F(j,d))))) for j in range(d+1))
        rnd=max(norm2(sub(cp[0],leaf['a'])),norm2(sub(cp[-1],leaf['b'])))
        if dev!=leaf['deviation2'] or rnd!=leaf['rounding2'] or not _sqrt_sum_le(dev,rnd,leaf['epsilon']):return False
        if i and (leaves[i-1]['t1']!=leaf['t0'] or leaves[i-1]['b']!=leaf['a']):return False
    return True


def bezier_length_bounds(controls,depth=6,precision_bits=160):
    """Certified lower/upper rational bounds; not necessarily one RN cell."""
    if not 0<=depth<=18:raise ValueError('reference depth cap')
    def leaf(cp):
        lower=sqrt_bounds(norm2(sub(cp[-1],cp[0])),precision_bits)[0]
        upper=sum((sqrt_bounds(norm2(sub(b,a)),precision_bits)[1] for a,b in zip(cp,cp[1:])),F(0))
        return lower,upper
    def visit(cp,d):
        if not d:return leaf(cp)
        a,b=split_bezier(cp);la,ua=visit(a,d-1);lb,ub=visit(b,d-1)
        return la+lb,ua+ub
    return visit(list(map(point,controls)),depth)


def uniform_t_samples(controls,count):
    if count<2:raise ValueError('at least two per-segment samples')
    return [(F(j,count-1),bezier_bernstein(controls,F(j,count-1))) for j in range(count)]


def _exact_segment_length(a,b):
    q=norm2(sub(point(b),point(a)));sn,sd=math.isqrt(q.numerator),math.isqrt(q.denominator)
    if sn*sn!=q.numerator or sd*sd!=q.denominator:
        raise NotImplementedError('this exact polyline helper requires rational segment lengths')
    return F(sn,sd)


def _polyline_lengths(points,closed=False):
    pp=list(map(point,points))
    if not pp:return pp,[],F(0)
    if closed and pp[-1]!=pp[0]:pp=pp+[pp[0]]
    lengths=[_exact_segment_length(a,b) for a,b in zip(pp,pp[1:])]
    return pp,lengths,sum(lengths,F(0))


def _at_length(pp,lengths,s):
    if not pp:raise ValueError('NoSolution: empty polyline')
    L=sum(lengths,F(0));s=frac(s)
    if not 0<=s<=L:raise ValueError('arc coordinate')
    if not L:return pp[0],0,F(0)
    cumulative=F(0)
    for i,z in enumerate(lengths):
        # Interior boundaries select the following positive-length segment.
        if z and (s<cumulative+z or s==L and cumulative+z==L):
            t=(s-cumulative)/z
            return add(mul(pp[i],1-t),mul(pp[i+1],t)),i,t
        cumulative+=z
    raise AssertionError('unreachable arc position')


def polyline_arc_samples(points,count,closed=False):
    if count<1:raise ValueError('count')
    pp,lengths,L=_polyline_lengths(points,closed)
    if not pp:return []
    ss=[F(0)] if count==1 else [F(j,count if closed else count-1)*L for j in range(count)]
    return [(s,*_at_length(pp,lengths,s)) for s in ss]


def polyline_spacing_samples(points,spacing,closed=False,include_open_end=True):
    delta=frac(spacing)
    if delta<=0:raise ValueError('spacing')
    pp,lengths,L=_polyline_lengths(points,closed)
    if not pp:return []
    if not L:return [(F(0),pp[0],0,F(0))]
    ss=[];s=F(0)
    while s<L:ss.append(s);s+=delta
    if not closed and include_open_end:ss.append(L)
    return [(s,*_at_length(pp,lengths,s)) for s in ss]


def trim_polyline(points,start,end):
    pp,lengths,L=_polyline_lengths(points);a,b=frac(start),frac(end)
    if not 0<=a<=b<=L:raise ValueError('trim interval')
    if not pp or a==b:return []
    out=[_at_length(pp,lengths,a)[0]];s=F(0)
    for p,z in zip(pp[1:],lengths):
        s+=z
        if a<s<b:out.append(p)
    out.append(_at_length(pp,lengths,b)[0]);return out


def dash_intervals(length,pattern,offset=0):
    L=frac(length);p=list(map(frac,pattern))
    if L<0 or not p or min(p)<0 or sum(p)<=0:raise ValueError('dash domain')
    if len(p)%2:p=p+p
    phase=frac(offset)%sum(p);i=0
    while p[i]==0 or phase>=p[i]:
        if p[i]:phase-=p[i]
        i=(i+1)%len(p)
    remaining=p[i]-phase;s=F(0);out=[]
    while s<L:
        end=min(L,s+remaining)
        if i%2==0 and end>s:
            if out and out[-1][1]==s:out[-1]=(out[-1][0],end)
            else:out.append((s,end))
        s=end;i=(i+1)%len(p)
        while p[i]==0:i=(i+1)%len(p)
        remaining=p[i]
    return out


def dash_polyline(points,pattern,offset=0):
    _,_,L=_polyline_lengths(points)
    return [trim_polyline(points,a,b) for a,b in dash_intervals(L,pattern,offset)]


def simplify_polyline(points,epsilon,locked=()):
    pp,lengths,L=_polyline_lengths(points);epsilon=frac(epsilon)
    if epsilon<0:raise ValueError('epsilon')
    if len(pp)<3:return pp
    fixed={0,len(pp)-1,*locked}
    if any(i<0 or i>=len(pp) for i in fixed):raise ValueError('locked index')
    cumulative=[F(0)]
    for z in lengths:cumulative.append(cumulative[-1]+z)
    def recurse(a,b):
        locks=sorted(i for i in fixed if a<i<b)
        if locks:
            idx=locks[0];return recurse(a,idx)[:-1]+recurse(idx,b)
        if b<=a+1:return [a,b]
        width=cumulative[b]-cumulative[a]
        values=[]
        for i in range(a+1,b):
            t=(cumulative[i]-cumulative[a])/width if width else F(0)
            q=add(mul(pp[a],1-t),mul(pp[b],t))
            values.append((norm2(sub(pp[i],q)),i))
        err,idx=max(values,key=lambda x:(x[0],-x[1]))
        if err<=epsilon*epsilon:return [a,b]
        return recurse(a,idx)[:-1]+recurse(idx,b)
    return [pp[i] for i in recurse(0,len(pp)-1)]


def cubic_candidate_checks(controls,source_endpoints):
    # Only an analytic necessary check, NOT a Hausdorff certifier/fitter.
    return len(controls)==4 and point(controls[0])==point(source_endpoints[0]) and point(controls[-1])==point(source_endpoints[-1])


def chaikin(points,iterations=1,closed=False):
    if not 0<=iterations<=12:raise ValueError('iterations')
    current=[tuple(v) for v in points]
    for _ in range(iterations):
        if len(current)<2:break
        edges=list(zip(current,current[1:]))+([(current[-1],current[0])] if closed else [])
        out=[] if closed else [current[0]]
        for a,b in edges:
            a,b=point(a),point(b)
            out.extend(tuple(round_fraction(v) for v in q) for q in (mul(add(mul(a,3),b),F(1,4)),mul(add(a,mul(b,3)),F(1,4))))
        if not closed:out.append(current[-1])
        current=out
    return current


def reverse_polyline(points,closed=False):
    return ([points[0]]+list(reversed(points[1:]))) if closed and points else list(reversed(points))


def concat_paths(paths):return [subpath for path in paths for subpath in path]


def split_subpaths(paths,groups,group_count):
    if len(paths)!=len(groups) or group_count<1 or any(not 0<=g<group_count for g in groups):raise ValueError('groups')
    out=[];offsets=[0]
    for g in range(group_count):out.extend(p for p,k in zip(paths,groups) if k==g);offsets.append(len(out))
    return out,offsets


def affine_transform(points,matrix):
    if len(matrix)!=2 or any(len(row)!=3 for row in matrix):raise ValueError('2x3 matrix')
    A=[list(map(frac,row)) for row in matrix]
    if A==[[F(1),F(0),F(0)],[F(0),F(1),F(0)]]:return [tuple(p) for p in points]
    return [tuple(round_fraction(A[j][0]*frac(p[0])+A[j][1]*frac(p[1])+A[j][2]) for j in range(2)) for p in points]


def validate_core_fixture(verbs,controls,control_offsets,subpath_offsets,closed):
    """Logical M/L/Q/C/Z fixture only; does not validate ABI associations."""
    arity={'M':1,'L':1,'Q':2,'C':3,'Z':0}
    pp=list(map(point,controls))
    if len(control_offsets)!=len(verbs)+1 or not control_offsets or control_offsets[0]!=0 or control_offsets[-1]!=len(pp):raise ValueError('control offsets')
    if len(subpath_offsets)!=len(closed)+1 or not subpath_offsets or subpath_offsets[0]!=0 or subpath_offsets[-1]!=len(verbs):raise ValueError('subpath offsets')
    for i,v in enumerate(verbs):
        if v not in arity or control_offsets[i+1]-control_offsets[i]!=arity[v]:raise ValueError('arity')
    for a,b,c in zip(subpath_offsets,subpath_offsets[1:],closed):
        if not a<b or verbs[a]!='M' or any(v=='M' for v in verbs[a+1:b]):raise ValueError('subpath M')
        if any(v=='Z' for v in verbs[a:b-1]) or bool(verbs[b-1]=='Z')!=bool(c):raise ValueError('closed/Z')
    return True


def validate_arc_fixture(center,u,v,start,sweep=None,direction=None):
    center,u,v=map(point,(center,u,v));frac(start)
    if not cross(u,v):raise ValueError('degenerate ellipse axes')
    if direction is not None:
        if direction not in (-1,1) or sweep is not None:raise ValueError('fullturn direction')
    elif sweep is None or not 0<abs(float(sweep))<2*math.pi:
        # MEASURED boundary check only; exact nonzero test is rational.
        raise ValueError('arc sweep')
    return True


def polygon_area(contour):
    p=list(map(point,contour))
    return sum((cross(a,b) for a,b in zip(p,p[1:]+p[:1])),F(0))/2 if p else F(0)


def _edges(contours):
    edges=[]
    for contour in contours:
        p=list(map(point,contour))
        edges.extend((a,b) for a,b in zip(p,p[1:]+p[:1]) if a!=b)
    return edges


def _on_segment(p,a,b):
    return not orientation(a,b,p) and min(a[0],b[0])<=p[0]<=max(a[0],b[0]) and min(a[1],b[1])<=p[1]<=max(a[1],b[1])


def segment_intersections(a,b,c,d):
    a,b,c,d=map(point,(a,b,c,d));r,s=sub(b,a),sub(d,c);den=cross(r,s)
    if den:
        t=cross(sub(c,a),s)/den;u=cross(sub(c,a),r)/den
        return [add(a,mul(r,t))] if 0<=t<=1 and 0<=u<=1 else []
    if cross(sub(c,a),r):return []
    return sorted({p for p in (a,b,c,d) if _on_segment(p,a,b) and _on_segment(p,c,d)})


def point_inside(p,contours,fill_rule='nonzero',boundary=True):
    p=point(p);w=0
    if fill_rule not in ('nonzero','evenodd'):raise ValueError('fill rule')
    for a,b in _edges(contours):
        if _on_segment(p,a,b):return boundary
        o=orientation(a,b,p)
        if a[1]<=p[1]<b[1] and o>0:w+=1
        elif b[1]<=p[1]<a[1] and o<0:w-=1
    return w!=0 if fill_rule=='nonzero' else w%2!=0


def polygon_pixel_area(contours,pixel=(0,0),fill_rule='nonzero'):
    """Exact rational area via vertical slabs, not sample-count AA.

    Handles arbitrary finite polygon contours, overlap, holes, shared/collinear
    edges and self-crossings; all edge events are retained with exact predicates.
    """
    if fill_rule not in ('nonzero','evenodd'):raise ValueError('fill rule')
    edges=_edges(contours);x0,y0=map(frac,pixel);x1,y1=x0+1,y0+1
    xs={x0,x1}
    for a,b in edges:
        xs.update(x for x in (a[0],b[0]) if x0<x<x1)
        if a[1]!=b[1]:
            for y in (y0,y1):
                t=(y-a[1])/(b[1]-a[1])
                if 0<t<1:
                    x=a[0]+t*(b[0]-a[0])
                    if x0<x<x1:xs.add(x)
    for i,(a,b) in enumerate(edges):
        for c,d in edges[:i]:
            for p in segment_intersections(a,b,c,d):
                if x0<p[0]<x1:xs.add(p[0])
    xs=sorted(xs);area=F(0)
    def val(line,x):return line[0]*x+line[1]
    for xl,xr in zip(xs,xs[1:]):
        xm=(xl+xr)/2;groups={}
        for a,b in edges:
            if min(a[0],b[0])<xm<max(a[0],b[0]):
                m=(b[1]-a[1])/(b[0]-a[0]);line=(m,a[1]-m*a[0])
                groups[line]=groups.get(line,0)+(1 if b[0]>a[0] else -1)
        lines=sorted(groups,key=lambda line:val(line,xm));w=0;previous=None
        for line in lines:
            inside=(w!=0) if fill_rule=='nonzero' else (w%2!=0)
            if previous is not None and inside:
                low=previous if val(previous,xm)>y0 else (F(0),y0)
                high=line if val(line,xm)<y1 else (F(0),y1)
                if val(high,xm)>val(low,xm):
                    area+=(xr-xl)*(val(high,xl)-val(low,xl)+val(high,xr)-val(low,xr))/2
            w+=groups[line];previous=line
        if w:raise AssertionError('closed contours must have zero final winding')
    if not 0<=area<=1:raise AssertionError('area outside unit pixel')
    return area


def _split_arrangement(edges):
    cuts=[{a,b} for a,b in edges]
    for i,(a,b) in enumerate(edges):
        for j,(c,d) in enumerate(edges[:i]):
            hits=segment_intersections(a,b,c,d);cuts[i].update(hits);cuts[j].update(hits)
    pieces=set()
    for (a,b),pp in zip(edges,cuts):
        d=sub(b,a);ordered=sorted(pp,key=lambda p:dot(sub(p,a),d))
        for u,v in zip(ordered,ordered[1:]):
            if u!=v:pieces.add(tuple(sorted((u,v))))
    return sorted(pieces)


def _side_points(a,b,all_edges):
    mid=mul(add(a,b),F(1,2));v=sub(b,a);normal=(-v[1],v[0]);n2=norm2(normal)
    distances=[point_segment_distance2(mid,c,d)[0] for c,d in all_edges if not _on_segment(mid,c,d)]
    limit=min(distances) if distances else F(1)
    if limit<=0:raise AssertionError('unsplit crossing at edge midpoint')
    step=F(1)
    while 4*step*step*n2>=limit:step/=2
    return add(mid,mul(normal,step)),sub(mid,mul(normal,step))


def region_boundary(contours,fill_rule='nonzero'):
    return boolean_boundary(contours,[],op='union',fill_rule_a=fill_rule)


def boolean_boundary(a,b,op='union',fill_rule_a='nonzero',fill_rule_b='nonzero'):
    if op not in ('union','intersection','difference','xor'):raise ValueError('boolean op')
    edges=_edges(a)+_edges(b)
    def inside(p):
        x=point_inside(p,a,fill_rule_a,False);y=point_inside(p,b,fill_rule_b,False)
        return (x or y) if op=='union' else ((x and y) if op=='intersection' else (x and not y if op=='difference' else x!=y))
    result=[]
    for u,v in _split_arrangement(edges):
        left,right=_side_points(u,v,edges);li,ri=inside(left),inside(right)
        if li!=ri:result.append((u,v) if li else (v,u))
    return sorted(result)


def _angle_compare(a,b):
    def half(v):return 0 if (v[1]>0 or v[1]==0 and v[0]>=0) else 1
    ha,hb=half(a),half(b)
    if ha!=hb:return -1 if ha<hb else 1
    z=cross(a,b)
    return -1 if z>0 else (1 if z<0 else 0)


def canonical_polygon(contour):
    pp=list(map(point,contour))
    if len(pp)>1 and pp[0]==pp[-1]:pp.pop()
    changed=True
    while changed and len(pp)>2:
        changed=False
        for i in range(len(pp)):
            a,b,c=pp[i-1],pp[i],pp[(i+1)%len(pp)]
            if not orientation(a,b,c) and _on_segment(b,a,c):
                pp.pop(i);changed=True;break
    if len(pp)<3 or not polygon_area(pp):return []
    candidates=[pp[i:]+pp[:i] for i,v in enumerate(pp) if v==min(pp)]
    return min(candidates)


def boundary_contours(edges):
    if not edges:return []
    outgoing={}
    for i,(a,b) in enumerate(edges):outgoing.setdefault(a,[]).append(i)
    nxt={}
    for i,(a,b) in enumerate(edges):
        opts=outgoing.get(b,[])
        if not opts:raise ArithmeticError('open regularized boundary')
        reverse=sub(a,b)
        # predecessor of reverse in CCW order = clockwise successor for left face.
        items=[(sub(edges[j][1],b),j) for j in opts]+[(reverse,-1)]
        items.sort(key=cmp_to_key(lambda x,y:_angle_compare(x[0],y[0]) or ((x[1]>y[1])-(x[1]<y[1]))))
        k=next(k for k,x in enumerate(items) if x[1]==-1)
        nxt[i]=items[(k-1)%len(items)][1]
    used=set();out=[]
    for first in range(len(edges)):
        if first in used:continue
        cycle=[];i=first
        while i not in used:
            used.add(i);cycle.append(edges[i][0]);i=nxt[i]
        if i!=first:raise ArithmeticError('non-permutation boundary traversal')
        cc=canonical_polygon(cycle)
        if cc:out.append(cc)
    return sorted(out)


def boolean_regions(a,b,op='union',fill_rule_a='nonzero',fill_rule_b='nonzero'):
    """Exact rational boundary fixture; Float64 publication is a separate gate."""
    return boundary_contours(boolean_boundary(a,b,op,fill_rule_a,fill_rule_b))


def boolean_axis_aligned(a,b,op='union'):
    def rect(r):
        x0,y0,x1,y1=map(frac,r)
        if x0>=x1 or y0>=y1:raise ValueError('positive rectangle')
        return [(x0,y0),(x1,y0),(x1,y1),(x0,y1)]
    return boolean_regions([rect(a)],[rect(b)],op)


def publish_polygon_fixture(contours,epsilon):
    """Finite incidence/area/nesting checks; NOT a general isotopy certifier.

    Output witnesses and exact distances are useful rejection oracles. Passing
    this helper alone must not certify the full production topology contract.
    """
    epsilon=frac(epsilon)
    if epsilon<=0:raise ValueError('epsilon')
    src=[list(map(point,c)) for c in contours]
    out=[[point(tuple(round_fraction(v) for v in p)) for p in c] for c in src]
    mapping={p:q for c,d in zip(src,out) for p,q in zip(c,d)}
    if len(set(mapping.values()))!=len(mapping):raise ArithmeticError('InvalidQuality: merged distinct vertices')
    if any(norm2(sub(p,q))>epsilon*epsilon for p,q in mapping.items()):raise ArithmeticError('InvalidQuality: publication displacement')
    if any(polygon_area(a)*polygon_area(b)<=0 for a,b in zip(src,out)):raise ArithmeticError('InvalidQuality: contour orientation/area')
    ea,eb=_edges(src),_edges(out)
    if len(ea)!=len(eb):raise ArithmeticError('InvalidQuality: collapsed edge')
    for i,(a,b) in enumerate(ea):
        for j,(c,d) in enumerate(ea[:i]):
            old=segment_intersections(a,b,c,d);new=segment_intersections(*eb[i],*eb[j])
            if set(new)!={mapping[p] for p in old if p in mapping} or any(p not in mapping for p in old):
                raise ArithmeticError('InvalidQuality: incidence changed or source not arranged')
    # Check bounded-loop nesting using a safe interior sample adjacent to edge 0.
    def nesting(cs):
        ee=_edges(cs);result=[]
        for c in cs:
            left,right=_side_points(c[0],c[1],ee)
            sample=left if polygon_area(c)>0 else right
            result.append(tuple(point_inside(sample,[d],boundary=False) for d in cs))
        return result
    if nesting(src)!=nesting(out):raise ArithmeticError('InvalidQuality: loop nesting changed')
    return out


def closest_polyline(p,contours):
    p=point(p);candidates=[]
    for si,contour in enumerate(contours):
        pp=list(map(point,contour))
        if len(pp)==1:candidates.append((norm2(sub(p,pp[0])),si,0,F(0),pp[0]))
        for j,(a,b) in enumerate(zip(pp,pp[1:])):
            q,t,c=point_segment_distance2(p,a,b);candidates.append((q,si,j,t,c))
    if not candidates:raise ValueError('NoSolution')
    return min(candidates,key=lambda v:v[:4])


def region_sdf(p,contours,fill_rule='nonzero',max_distance=None,dtype='float64'):
    boundary=region_boundary(contours,fill_rule)
    if not boundary:
        if max_distance is None:raise ValueError('NoSolution: empty regularized region')
        return round_fraction(frac(max_distance),dtype)
    p=point(p);d2=min(point_segment_distance2(p,a,b)[0] for a,b in boundary)
    if not d2:return 0.0
    # Use the exposed boundary, not raw source edges: a query on an internal
    # shared source edge can still be strictly inside the regularized union.
    sign=-1 if point_inside(p,boundary_contours(boundary),'nonzero',False) else 1
    if max_distance is not None and d2>=frac(max_distance)**2:return round_fraction(sign*frac(max_distance),dtype)
    return sign*sqrt_fraction(d2,dtype)


def straight_stroke_rectangles(a,b,width,cap='butt'):
    """Exact axis-aligned butt/square fixture; round/corner union not included."""
    a,b=point(a),point(b);r=frac(width)/2
    if r<0 or cap not in ('butt','square'):raise ValueError('fixture supports butt/square')
    if not r:return []
    if a==b:
        return [] if cap=='butt' else [[(a[0]-r,a[1]-r),(a[0]+r,a[1]-r),(a[0]+r,a[1]+r),(a[0]-r,a[1]+r)]]
    e=r if cap=='square' else F(0)
    if a[1]==b[1]:x0,x1=min(a[0],b[0])-e,max(a[0],b[0])+e;y0,y1=a[1]-r,a[1]+r
    elif a[0]==b[0]:x0,x1=a[0]-r,a[0]+r;y0,y1=min(a[1],b[1])-e,max(a[1],b[1])+e
    else:raise NotImplementedError('fixture is axis-aligned')
    return [[(x0,y0),(x1,y0),(x1,y1),(x0,y1)]]


def variable_sweep_contains(p,a,b,r0,r1):
    """Exact existential membership in linearly varying disc sweep.

    Minimize ||p-a-t(b-a)||^2-(r0+t(r1-r0))^2 on [0,1].
    """
    p,a,b=map(point,(p,a,b));r0,r1=frac(r0),frac(r1)
    if min(r0,r1)<0:raise ValueError('negative radius')
    v,q=sub(b,a),sub(p,a);dr=r1-r0
    A=norm2(v)-dr*dr;B=-2*(dot(q,v)+r0*dr);C=norm2(q)-r0*r0
    ts=[F(0),F(1)]
    if A>0 and 0<-B/(2*A)<1:ts.append(-B/(2*A))
    return min(A*t*t+B*t+C for t in ts)<=0


polygon_coverage=polygon_pixel_area

def rectangle_sdf(p,center,half_size,dtype='float64'):
    from exact import rectangle_sdf as by_bounds
    c,h=point(center),point(half_size)
    if min(h)<=0:raise ValueError('positive half size')
    return by_bounds(p,(c[0]-h[0],c[1]-h[1],c[0]+h[0],c[1]+h[1]),dtype)
