"""Exact selection, type-7 quantiles, integer histograms and native CLAHE v1."""
from .precision import *
from .point import bounded

def flatten(x):
    if isinstance(x,(list,tuple)):
        for a in x:yield from flatten(a)
    else:yield x

def selected_pairs(feature,selection,mode='binary'):
    f=list(flatten(feature));s=list(flatten(selection))
    if len(f)!=len(s):raise DomainError('selection shape')
    if mode not in ('binary','weighted'):raise DomainError('selection mode')
    for v in s:
        if not 0<=frac(v)<=1 or (mode=='binary' and v not in (0,1)):raise DomainError('selection domain')
    return [(f[i],frac(b)) for i,b in enumerate(s) if b]
def selected_values(feature,selection):return [x for x,w in selected_pairs(feature,selection)]
def count_selection(selection):
    s=list(flatten(selection))
    if any(v not in (0,1) for v in s):raise DomainError('binary selection')
    return sum(s)
def quantiles(feature,selection,probabilities,mode='binary'):
    raw=selected_pairs(feature,selection,mode);vals=[(frac(x),i,x,w) for i,(x,w) in enumerate(raw)];vals.sort(key=lambda t:(t[0],t[1]))
    p=list(map(frac,probabilities));n=len(vals)
    if not 1<=len(p)<=64 or any(not 0<=q<=1 for q in p):raise DomainError('quantile parameters')
    if n==0:
        return [0.0]*len(p),0,0.0
    mass=sum(v[3] for v in vals)
    out=[]
    for q in p:
        if mode=='weighted':
            cumulative=F(0)
            for value in vals:
                cumulative+=value[3]
                if cumulative>=q*mass:
                    out.append(finite_copy(value[2],64));break
            continue
        h=(n-1)*q;i=h.numerator//h.denominator;t=h-i
        out.append(finite_copy(vals[i][2],64) if t==0 else rn((1-t)*vals[i][0]+t*vals[min(i+1,n-1)][0],64))
    return out,n,rn(mass,64)

def exposure_parameters(quantile,count,target=.18,ev_min=-16,ev_max=16,invalid='identity'):
    q,t,lo,hi=map(frac,(quantile,target,ev_min,ev_max))
    if not isinstance(count,int) or not 0<=count<=2**40 or t<=0 or lo>hi or invalid not in ('identity','error'):raise DomainError('exposure parameters')
    if count==0 or q<=0:
        if invalid=='error':raise DomainError('invalid metering statistic')
        return 0.0,1
    return rn(clamp(log2(t/q),lo,hi),64),0

def levels_parameters(quantiles,count,gamma=1,out0=0,out1=1,invalid='identity'):
    if len(quantiles)!=2:raise DomainError('quantile shape')
    lo,hi=map(frac,quantiles);g,a,b=map(frac,(gamma,out0,out1))
    if not isinstance(count,int) or not 0<=count<=2**40 or lo>hi or g<=0 or invalid not in ('identity','error'):raise DomainError('levels parameters')
    if count==0 or lo==hi:
        if invalid=='error':raise DomainError('invalid metering statistic')
        return [0.0,1.0,1.0,0.0,1.0],1
    return [rn(x,64) for x in (lo,hi,g,a,b)],0

def bin_index(x,bins=256,edge='clamp'):
    if not isinstance(bins,int) or not 2<=bins<=65536:raise DomainError('bins')
    u=bounded(x,edge);return min(bins-1,(u*bins).numerator//(u*bins).denominator)
def histogram(feature,selection,bins=256,edge='clamp',mode='binary'):
    # Validate static bins even for an empty selection.
    bin_index(0,bins,edge);counts=[F(0)]*bins;v=selected_pairs(feature,selection,mode)
    for x,w in v:counts[bin_index(x,bins,edge)]+=w
    if len(v)>2**40:raise DomainError('count limit')
    return [int(x) if mode=='binary' else rn(x,64) for x in counts],len(v),rn(sum(counts),64)
def _counts(counts,mode='binary'):
    c=list(map(frac,counts))
    if mode not in ('binary','weighted') or not 2<=len(c)<=65536 or min(c)<0 or sum(c)>2**40:raise DomainError('histogram counts')
    if mode=='binary' and any(x.denominator!=1 for x in c):raise DomainError('integer counts')
    return c
def cdf(counts,mode='binary'):
    c=_counts(counts,mode);n=sum(c);nonzero=[i for i,x in enumerate(c) if x]
    if len(nonzero)<=1:return [rn(F(i,len(c)-1),64) for i in range(len(c))],1
    first=c[nonzero[0]];prefix=0;out=[]
    for x in c:
        prefix+=x;out.append(rn(max(F(0),F(prefix-first,n-first)),64))
    return out,0

def validate_map(mapping,identity):
    m=list(map(frac,mapping))
    if not 2<=len(m)<=65536 or identity not in (0,1) or any(not 0<=x<=1 for x in m) or any(a>b for a,b in zip(m,m[1:])):raise DomainError('invalid map')
    return m

def hist_apply(x,mapping,identity,edge='clamp',dtype=64):
    m=validate_map(mapping,identity);u=bounded(x,edge)
    if identity:return finite_copy(x,dtype) if frac(x)==u else rn(u,dtype)
    return rn(m[bin_index(u,len(m),'reject')],dtype)

def clahe_map(counts,clip_factor=2,mode='binary'):
    c=_counts(counts,mode);f=frac(clip_factor);n=sum(c);B=len(c)
    if f<1:raise DomainError('clip_factor <1')
    if sum(x>0 for x in c)<=1:return [rn(F(i,B-1),64) for i in range(B)],1
    limit=max(1,(f*n/B).numerator//(f*n/B).denominator) if mode=='binary' else f*n/B
    clipped=[min(x,limit) for x in c]
    if mode=='binary':
        q,r=divmod(n-sum(clipped),B);clipped=[x+q+(i<r) for i,x in enumerate(clipped)]
    else:
        delta=(n-sum(clipped))/B;clipped=[x+delta for x in clipped]
    prefix=0;out=[]
    for x in clipped:prefix+=x;out.append(rn(F(prefix,n),64))
    return out,0

def clahe_maps(feature,selection,cell=(64,64),bins=256,clip_factor=2,edge='clamp',mode='binary'):
    H=len(feature);W=len(feature[0]) if H else 0;ch,cw=cell
    if not H or not W or ch<1 or cw<1 or len(selection)!=H or any(len(r)!=W for r in feature+selection):raise DomainError('image/cell shape')
    maps=[];flags=[]
    for y in range(0,H,ch):
        mr=[];fr=[]
        for x in range(0,W,cw):
            v=[r[x:min(W,x+cw)] for r in feature[y:min(H,y+ch)]];s=[r[x:min(W,x+cw)] for r in selection[y:min(H,y+ch)]]
            h,_,_=histogram(v,s,bins,edge,mode);m,i=clahe_map(h,clip_factor,mode);mr.append(m);fr.append(i)
        maps.append(mr);flags.append(fr)
    return maps,flags

def centers(size,cell):return [F(i+min(size,i+cell),2) for i in range(0,size,cell)]
def _brackets(center,position):
    if position<=center[0]:return [(0,F(1))]
    if position>=center[-1]:return [(len(center)-1,F(1))]
    for i in range(len(center)-1):
        if center[i]<=position<=center[i+1]:
            t=(position-center[i])/(center[i+1]-center[i]);return [(i,1-t),(i+1,t)]
    raise AssertionError('unreachable')
def clahe_apply(x,y,pixel,maps,identity,shape,cell=(64,64),edge='clamp',dtype=64):
    H,W=shape;ch,cw=cell
    if not(0<=x<W and 0<=y<H and ch>0 and cw>0):raise DomainError('coordinate/cell')
    cy,cx=centers(H,ch),centers(W,cw)
    if len(maps)!=len(cy) or len(identity)!=len(cy) or any(len(r)!=len(cx) for r in maps+identity):raise DomainError('map grid shape')
    u=bounded(pixel,edge);total=F(0);all_id=True
    B=len(maps[0][0])
    for iy,wy in _brackets(cy,F(2*y+1,2)):
        for ix,wx in _brackets(cx,F(2*x+1,2)):
            m=validate_map(maps[iy][ix],identity[iy][ix])
            if len(m)!=B:raise DomainError('inconsistent bins')
            if wx*wy==0:continue
            v=u if identity[iy][ix] else m[bin_index(u,B,'reject')]
            all_id &= bool(identity[iy][ix]);total+=wx*wy*v
    return finite_copy(pixel,dtype) if all_id and u==frac(pixel) else rn(total,dtype)
