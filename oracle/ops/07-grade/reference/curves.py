"""Exact-rational interpolation, periodic curves, LUTs and mesh displacement."""
from bisect import bisect_right
from itertools import product
from .precision import *

def _knots(knots,values):
    k=list(map(frac,knots));v=list(map(frac,values))
    if not 2<=len(k)<=65536 or len(k)!=len(v) or any(a>=b for a,b in zip(k,k[1:])):raise DomainError('curve shape/order')
    return k,v

def _cell(k,x,edge):
    x=frac(x)
    if edge not in ('reject','clamp','linear_extrapolate'):raise DomainError('edge')
    if edge=='reject' and not k[0]<=x<=k[-1]:raise DomainError('query outside grid')
    if edge=='clamp':x=max(k[0],min(k[-1],x))
    j=max(0,min(len(k)-2,bisect_right(k,x)-1));return j,(x-k[j])/(k[j+1]-k[j]),x

def _signed_zero(endpoints):return all(float(x)==0 and math.copysign(1,float(x))<0 for x in endpoints)
def linear(query,knots,values,edge='reject',dtype=64):
    k,v=_knots(knots,values);j,t,q=_cell(k,query,edge)
    if q in k:return finite_copy(values[k.index(q)],dtype)
    y=(1-t)*v[j]+t*v[j+1]
    return round_fraction(y,dtype,negative_zero=y==0 and _signed_zero(values[j:j+2]))

def _slopes(k,v):
    h=[b-a for a,b in zip(k,k[1:])];d=[(b-a)/w for a,b,w in zip(v,v[1:],h)]
    if len(k)==2:return d*2
    def sign(x):return (x>0)-(x<0)
    def endpoint(h0,h1,d0,d1):
        e=((2*h0+h1)*d0-h0*d1)/(h0+h1)
        if sign(e)!=sign(d0):return F(0)
        return 3*d0 if sign(d0)!=sign(d1) and abs(e)>3*abs(d0) else e
    m=[endpoint(h[0],h[1],d[0],d[1])]
    for i in range(1,len(k)-1):
        if d[i-1]*d[i]<=0:m.append(F(0))
        else:
            w1=2*h[i]+h[i-1];w2=h[i]+2*h[i-1];m.append((w1+w2)/(w1/d[i-1]+w2/d[i]))
    return m+[endpoint(h[-1],h[-2],d[-1],d[-2])]
def pchip(query,knots,values,edge='reject',dtype=64):
    k,v=_knots(knots,values);j,t,q=_cell(k,query,edge)
    if q in k:return finite_copy(values[k.index(q)],dtype)
    m=_slopes(k,v)
    if q<k[0]:y=v[0]+(q-k[0])*m[0]
    elif q>k[-1]:y=v[-1]+(q-k[-1])*m[-1]
    else:
        h=k[j+1]-k[j]
        y=(2*t**3-3*t*t+1)*v[j]+(t**3-2*t*t+t)*h*m[j]+(-2*t**3+3*t*t)*v[j+1]+(t**3-t*t)*h*m[j+1]
    return round_fraction(y,dtype,negative_zero=y==0 and _signed_zero(values[j:j+2]))
def ramp(query,knots,colors,edge='clamp',dtype=64):
    if len(colors)!=len(knots) or any(len(c)!=3 for c in colors):raise DomainError('ramp shape')
    return [linear(query,knots,[c[i] for c in colors],edge,dtype) for i in range(3)]
def periodic(query,knots,values,period=2,dtype=64,nonnegative=False):
    k,v=_knots(knots,values);p=frac(period)
    if p<=0 or k[0]!=0 or k[-1]>=p or (nonnegative and min(v)<0):raise DomainError('periodic table')
    return linear(frac(query)%p,k+[p],list(values)+[values[0]],'reject',dtype)
def grid_axis(axis,n):
    if len(axis)!=2 or not 2<=n<=256:raise DomainError('axis shape')
    a,b=map(frac,axis)
    if a==b:raise DomainError('collapsed endpoints')
    return [((n-1-j)*a+j*b)/(n-1) for j in range(n)]
def _axis_cell(k,q,edge):
    if k[0]<k[-1]:return _cell(k,q,edge)[:2]
    j,t,_=_cell([-x for x in k],-frac(q),edge);return j,t

def _table3(table):
    nr=len(table);ng=len(table[0]) if nr else 0;nb=len(table[0][0]) if ng else 0
    if any(not 2<=n<=256 for n in (nr,ng,nb)):raise DomainError('table extents')
    for plane in table:
        if len(plane)!=ng:raise DomainError('ragged table')
        for row in plane:
            if len(row)!=nb:raise DomainError('ragged table')
            for cell in row:
                if len(cell)!=3:raise DomainError('RGB table')
                for x in cell:frac(x)  # Oracle diagnostics, not runtime validation.
    return nr,ng,nb

def _lut(rgb,table,axis,edge,dtype,tetra):
    dims=_table3(table)
    if len(rgb)!=3 or len(axis)!=3 or edge not in ('clamp','reject'):raise DomainError('LUT signature')
    cells=[_axis_cell(grid_axis(row,n),q,edge) for row,n,q in zip(axis,dims,rgb)];j=[a[0] for a in cells];t=[a[1] for a in cells]
    vertices=[]
    if tetra:
        order=sorted(range(3),key=lambda i:(-t[i],i));a,b,c=[t[i] for i in order];weights=[1-a,a-b,b-c,c];loc=[0,0,0];vs=[tuple(loc)]
        for i in order:loc[i]=1;vs.append(tuple(loc))
        vertices=list(zip(vs,weights))
    else:
        for loc in product((0,1),repeat=3):
            w=F(1)
            for i in range(3):w*=t[i] if loc[i] else 1-t[i]
            vertices.append((loc,w))
    entries=[(table[j[0]+loc[0]][j[1]+loc[1]][j[2]+loc[2]],w) for loc,w in vertices if w>0]
    if len(entries)==1:return [finite_copy(x,dtype) for x in entries[0][0]]
    result=[]
    for c in range(3):
        y=sum(frac(v[c])*w for v,w in entries)
        result.append(round_fraction(y,dtype,negative_zero=y==0 and _signed_zero([v[c] for v,w in entries])))
    return result

def lut_trilinear(rgb,table,axis=((0,1),)*3,edge='reject',dtype=64):return _lut(rgb,table,axis,edge,dtype,False)
def lut_tetrahedral(rgb,table,axis=((0,1),)*3,edge='reject',dtype=64):return _lut(rgb,table,axis,edge,dtype,True)

def _mesh(ua,va,disp):
    u=list(map(frac,ua));v=list(map(frac,va))
    if not 2<=len(u)<=256 or not 2<=len(v)<=256 or any(a>=b for a,b in zip(u,u[1:])) or any(a>=b for a,b in zip(v,v[1:])):raise DomainError('mesh axes')
    if len(disp)!=len(u) or any(len(r)!=len(v) for r in disp) or any(len(p)!=2 for r in disp for p in r):raise DomainError('mesh shape')
    d=[[[frac(x) for x in p] for p in r] for r in disp];return u,v,d

def _bilinear(x,y,u,v,d,edge):
    i,t,_=_cell(u,x,edge);j,s,_=_cell(v,y,edge)
    return [sum(d[i+a][j+b][c]*(t if a else 1-t)*(s if b else 1-s) for a,b in product((0,1),repeat=2)) for c in range(2)]
def warper_ab(lab,u_axis,v_axis,displacement,edge='clamp',topology='allow',boundary='free',dtype=64):
    l,a,b=map(frac,lab);u,v,d=_mesh(u_axis,v_axis,displacement)
    if edge not in ('clamp','reject') or topology not in ('allow','positive_jacobian') or boundary not in ('free','pinned_zero'):raise DomainError('mesh mode')
    if boundary=='pinned_zero' and any(d[i][j]!=[0,0] for i in range(len(u)) for j in range(len(v)) if i in (0,len(u)-1) or j in (0,len(v)-1)):raise DomainError('unpinned boundary')
    if topology=='positive_jacobian':
        for i in range(len(u)-1):
            for j in range(len(v)-1):
                du,dv=u[i+1]-u[i],v[j+1]-v[j]
                for t,s in product((F(0),F(1)),repeat=2):
                    dx=[((1-s)*(d[i+1][j][c]-d[i][j][c])+s*(d[i+1][j+1][c]-d[i][j+1][c]))/du for c in range(2)]
                    dy=[((1-t)*(d[i][j+1][c]-d[i][j][c])+t*(d[i+1][j+1][c]-d[i+1][j][c]))/dv for c in range(2)]
                    if (1+dx[0])*(1+dy[1])-dy[0]*dx[1]<=0:raise DomainError('nonpositive local Jacobian')
    da,db=_bilinear(a,b,u,v,d,edge)
    return [finite_copy(lab[0],dtype),finite_copy(lab[1],dtype) if da==0 else rn(a+da,dtype),finite_copy(lab[2],dtype) if db==0 else rn(b+db,dtype)]
def warper_hc(lch,hue_axis,chroma_axis,displacement,edge='clamp',dtype=64,topology='allow'):
    l,c,h=map(frac,lch);u,v,d=_mesh(hue_axis,chroma_axis,displacement)
    if edge not in ('clamp','reject') or c<0 or u[0]!=0 or u[-1]>=2 or v[0]!=0:raise DomainError('polar mesh domain')
    if any(row[0]!=[0,0] for row in d):raise DomainError('nonzero achromatic displacement')
    if any(v[j]+d[i][j][1]<0 for i in range(len(u)) for j in range(len(v))):raise DomainError('negative mapped chroma')
    if topology not in ('allow','positive_jacobian'):raise DomainError('topology')
    if topology=='positive_jacobian':
        warper_ab([0,0,0],u+[F(2)],v,d+[d[0]],'clamp',topology,dtype=64)
    # Hue is always periodic; chroma alone obeys the edge mode.
    if edge=='reject' and not v[0]<=c<=v[-1]:raise DomainError('chroma out of range')
    dh,dc=_bilinear(h%2,c,u+[F(2)],v,d+[d[0]],'clamp')
    return [finite_copy(lch[0],dtype),finite_copy(lch[1],dtype) if dc==0 else rn(c+dc,dtype),finite_copy(lch[2],dtype) if dh==0 else rn(h+dh,dtype)]

def warper_cl(lch,chroma_axis,lightness_axis,displacement,edge='clamp',topology='allow',dtype=64):
    l,c,h=map(frac,lch);u,v,d=_mesh(chroma_axis,lightness_axis,displacement)
    if c<0 or u[0]!=0:raise DomainError('chroma domain')
    if any(p[0]!=0 for p in d[0]):raise DomainError('achromatic chroma displacement')
    if any(u[i]+d[i][j][0]<0 for i in range(len(u)) for j in range(len(v))):raise DomainError('mapped negative chroma')
    out=warper_ab([lch[2],lch[1],lch[0]],u,v,d,edge,topology,'free',dtype)
    return [out[2],out[1],out[0]]
