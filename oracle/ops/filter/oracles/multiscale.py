"""Independent, explicit-RN pyramid and wavelet algorithms for small fixtures.

Collections are JSON-friendly *test representations*, not the runtime ABI.
The three wavelet profiles are deliberately named, not aliases for a library.
"""
from __future__ import annotations
import copy
from .core import Q,frac,rn,zeros,shape,same_shape,sample,DomainError
from .spatial import separable
from .bounds import certify

BINOMIAL=[Q(1,16),Q(4,16),Q(6,16),Q(4,16),Q(1,16)]
PROFILES=('haar_mean_lifting_v1','cdf53_float_lifting_v1','stationary_haar_mean_v1')
ROLES=('LH_y','HL_x','HH')

def reduce_level(image,dtype='float64'):
    blurred=separable(image,BINOMIAL,BINOMIAL,(2,2),boundary='reflect_half',dtype=dtype)
    h,w=shape(image)
    return [[blurred[y][x] for x in range(0,w,2)] for y in range(0,h,2)]

def expand_level(coarse,target_shape,dtype='float64'):
    h,w=target_shape
    if shape(coarse)!=((h+1)//2,(w+1)//2):raise DomainError('expand target shape')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            total=Q()
            for dy in range(-2,3):
                for dx in range(-2,3):
                    if (y-dy)%2 or (x-dx)%2:continue
                    total+=4*BINOMIAL[dy+2]*BINOMIAL[dx+2]*sample(coarse,(y-dy)//2,(x-dx)//2,'reflect_half')
            out[y][x]=rn(total,dtype)
    return out

def _max_decimated_levels(h,w,cdf=False):
    n=0
    while (min(h,w)>=2 if cdf else max(h,w)>1):
        h,w=(h+1)//2,(w+1)//2;n+=1
    return n

def pyramid(image=None,levels=0,*,mode='laplacian',bands=None,dtype='float64'):
    if mode=='reconstruct':
        if bands is None or bands.get('profile')!='pyramid_binomial5_v1' or bands.get('kind')!='laplacian':raise DomainError('pyramid schema/profile')
        levels=bands['levels'];shapes=[tuple(s) for s in bands['shapes']]
        if len(shapes)!=levels+1 or tuple(bands['original_shape'])!=shapes[0]:raise DomainError('pyramid shape history')
        by={(b['level'],b['role']):b['values'] for b in bands['bands']}
        expected={(l,'detail') for l in range(levels)}|{(levels,'base')}
        if set(by)!=expected or len(by)!=len(bands['bands']):raise DomainError('pyramid role set')
        if any(shapes[l+1]!=((shapes[l][0]+1)//2,(shapes[l][1]+1)//2) for l in range(levels)):raise DomainError('pyramid shape progression')
        current=copy.deepcopy(by[(levels,'base')])
        if shape(current)!=shapes[-1]:raise DomainError('base shape')
        for l in range(levels-1,-1,-1):
            detail=by[(l,'detail')]
            if shape(detail)!=shapes[l]:raise DomainError('detail shape')
            ex=expand_level(current,shapes[l],dtype);h,w=shapes[l]
            current=[[rn(frac(detail[y][x])+frac(ex[y][x]),dtype) for x in range(w)] for y in range(h)]
        return current
    h,w=shape(image)
    if levels<0 or levels>_max_decimated_levels(h,w):raise DomainError('pyramid levels')
    if mode not in ('gaussian','laplacian'):raise DomainError('pyramid mode')
    gs=[copy.deepcopy(image)]
    for _ in range(levels):gs.append(reduce_level(gs[-1],dtype))
    result={'schema':'FilterBands/v1','profile':'pyramid_binomial5_v1','kind':mode,'levels':levels,'dtype':dtype,'original_shape':[h,w],'shapes':[list(shape(a)) for a in gs],'bands':[]}
    if mode=='gaussian':
        result['bands']=[{'level':l,'role':'gaussian','values':v} for l,v in enumerate(gs)]
    else:
        for l in range(levels):
            hh,ww=shape(gs[l]);ex=expand_level(gs[l+1],(hh,ww),dtype)
            values=[[rn(frac(gs[l][y][x])-frac(ex[y][x]),dtype) for x in range(ww)] for y in range(hh)]
            result['bands'].append({'level':l,'role':'detail','values':values})
        result['bands'].append({'level':levels,'role':'base','values':gs[-1]})
    return result

def _analyze_line(v,profile,offset,dtype):
    n=len(v);v=list(map(frac,v))
    if profile=='stationary_haar_mean_v1':
        return ([rn((v[i]+v[(i+offset)%n])/2,dtype) for i in range(n)],
                [rn((v[i]-v[(i+offset)%n])/2,dtype) for i in range(n)])
    e=v[::2];o=v[1::2]
    if profile=='haar_mean_lifting_v1':
        if n%2:o.append(e[-1])
        d=[rn(b-a,dtype) for a,b in zip(e,o)]
        return [rn(a+frac(b)/2,dtype) for a,b in zip(e,d)],d
    if n<2:return [float(v[0])],[]
    d=[rn(o[i]-(e[i]+e[min(i+1,len(e)-1)])/2,dtype) for i in range(len(o))]
    s=[rn(e[i]+(frac(d[max(i-1,0)])+frac(d[min(i,len(d)-1)]))/4,dtype) for i in range(len(e))]
    return s,d

def _synthesize_line(s,d,n,profile,dtype):
    s=list(map(frac,s));d=list(map(frac,d))
    if profile=='stationary_haar_mean_v1':
        if len(s)!=n or len(d)!=n:raise DomainError('stationary band size')
        return [rn(a+b,dtype) for a,b in zip(s,d)]
    if profile=='haar_mean_lifting_v1':
        if len(s)!=(n+1)//2 or len(d)!=len(s):raise DomainError('Haar band size')
        e=[rn(a-b/2,dtype) for a,b in zip(s,d)];o=[rn(b+frac(a),dtype) for a,b in zip(e,d)]
    else:
        if len(s)!=(n+1)//2 or len(d)!=n//2:raise DomainError('CDF band size')
        if n==1:return [float(s[0])]
        e=[rn(s[i]-(d[max(i-1,0)]+d[min(i,len(d)-1)])/4,dtype) for i in range(len(s))]
        o=[rn(d[i]+(frac(e[i])+frac(e[min(i+1,len(e)-1)]))/2,dtype) for i in range(len(d))]
    out=[]
    for i,a in enumerate(e):
        out.append(a)
        if i<len(o):out.append(o[i])
    return out[:n]

def _analyze_axis(image,axis,profile,offset,dtype):
    h,w=shape(image)
    if axis==1:
        pairs=[_analyze_line(row,profile,offset,dtype) for row in image]
        return [p[0] for p in pairs],[p[1] for p in pairs]
    pairs=[_analyze_line([image[y][x] for y in range(h)],profile,offset,dtype) for x in range(w)]
    return [[pairs[x][0][y] for x in range(w)] for y in range(len(pairs[0][0]))],[[pairs[x][1][y] for x in range(w)] for y in range(len(pairs[0][1]))]

def _synthesize_axis(low,high,n,axis,profile,dtype):
    if axis==1:
        if len(low)!=len(high):raise DomainError('row count')
        return [_synthesize_line(s,d,n,profile,dtype) for s,d in zip(low,high)]
    h,w=shape(low);hh,ww=shape(high)
    if w!=ww:raise DomainError('column count')
    columns=[_synthesize_line([low[y][x] for y in range(h)],[high[y][x] for y in range(hh)],n,profile,dtype) for x in range(w)]
    return [[columns[x][y] for x in range(w)] for y in range(n)]

def wavelet(image=None,levels=0,*,profile='haar_mean_lifting_v1',inverse=False,bands=None,dtype='float64'):
    if inverse:
        if bands is None or bands.get('schema')!='FilterBands/v1' or bands.get('profile') not in PROFILES:raise DomainError('wavelet schema')
        profile=bands['profile'];levels=bands['levels'];shapes=[tuple(s) for s in bands['shapes']]
        if len(shapes)!=levels+1 or tuple(bands['original_shape'])!=shapes[0]:raise DomainError('wavelet history')
        by={(b['level'],b['role']):b['values'] for b in bands['bands']}
        expected={(l,r) for l in range(1,levels+1) for r in ROLES}|{(levels,'LL')}
        if set(by)!=expected or len(by)!=len(bands['bands']):raise DomainError('wavelet roles')
        current=copy.deepcopy(by[(levels,'LL')])
        if shape(current)!=shapes[-1]:raise DomainError('LL shape')
        for l in range(levels,0,-1):
            h,w=shapes[l-1]
            if profile=='stationary_haar_mean_v1':expected_next=(h,w)
            else:expected_next=((h+1)//2,(w+1)//2)
            if shapes[l]!=expected_next:raise DomainError('wavelet shape progression')
            xl=_synthesize_axis(current,by[(l,'LH_y')],h,0,profile,dtype)
            xh=_synthesize_axis(by[(l,'HL_x')],by[(l,'HH')],h,0,profile,dtype)
            current=_synthesize_axis(xl,xh,w,1,profile,dtype)
        return current
    h,w=shape(image)
    if profile not in PROFILES or levels<0:raise DomainError('wavelet profile/levels')
    if profile=='stationary_haar_mean_v1':
        if levels>40 or (levels and 2**(levels-1)>2**((max(h,w)-1).bit_length())):raise DomainError('stationary offset limit')
    elif levels>_max_decimated_levels(h,w,profile=='cdf53_float_lifting_v1'):raise DomainError('wavelet levels')
    result={'schema':'FilterBands/v1','profile':profile,'dtype':dtype,'levels':levels,'original_shape':[h,w],'shapes':[[h,w]],'bands':[]}
    current=copy.deepcopy(image)
    for l in range(1,levels+1):
        offset=2**(l-1)
        xl,xh=_analyze_axis(current,1,profile,offset,dtype)
        ll,lh=_analyze_axis(xl,0,profile,offset,dtype);hl,hh=_analyze_axis(xh,0,profile,offset,dtype)
        for role,values in zip(ROLES,[lh,hl,hh]):result['bands'].append({'level':l,'role':role,'values':values})
        current=ll;result['shapes'].append(list(shape(ll)))
    result['bands'].append({'level':levels,'role':'LL','values':current})
    return result

def threshold_bands(bands,thresholds,*,mode='soft',dtype='float64'):
    if bands.get('profile') not in PROFILES or mode not in ('soft','hard'):raise DomainError('threshold profile')
    levels=bands['levels']
    if levels==0 or shape(thresholds)!=(levels,3):raise DomainError('positive threshold table shape')
    ts=[[frac(x) for x in r] for r in thresholds]
    if any(v<0 for row in ts for v in row):raise DomainError('negative threshold')
    out=copy.deepcopy(bands)
    for band in out['bands']:
        if band['role']=='LL':continue
        t=ts[band['level']-1][ROLES.index(band['role'])]
        if not t:continue
        values=band['values']
        for y,row in enumerate(values):
            for x,raw in enumerate(row):
                v=frac(raw)
                if mode=='hard':values[y][x]=0.0 if abs(v)<t else raw
                else:values[y][x]=0.0 if abs(v)<=t else rn((1 if v>0 else -1)*(abs(v)-t),dtype)
    return out

def _shift(image,dy,dx):
    h,w=shape(image)
    return [[image[(y+dy)%h][(x+dx)%w] for x in range(w)] for y in range(h)]

def cycle_spin(image,thresholds,*,levels=1,profile='haar_mean_lifting_v1',shifts=((0,0),),mode='soft',dtype='float64'):
    h,w=shape(image)
    if not shifts or len(set((y%h,x%w) for y,x in shifts))!=len(shifts):raise DomainError('cycle shifts')
    if levels==0:return copy.deepcopy(image)
    branches=[]
    for dy,dx in shifts:
        b=wavelet(_shift(image,dy,dx),levels,profile=profile,dtype=dtype)
        b=threshold_bands(b,thresholds,mode=mode,dtype=dtype)
        branches.append(_shift(wavelet(inverse=True,bands=b,dtype=dtype),-dy,-dx))
    return [[rn(sum((frac(a[y][x]) for a in branches),Q())/len(branches),dtype) for x in range(w)] for y in range(h)]

def local_laplacian(image,*,levels=1,edge_threshold=1,detail_exponent=1,edge_slope=1,dtype='float64'):
    h,w=shape(image);s,a,b=map(frac,(edge_threshold,detail_exponent,edge_slope))
    if s<=0 or a<=0 or b<0:raise DomainError('local Laplacian parameters')
    if a==1 and b==1:return copy.deepcopy(image)
    if h*w>36:raise DomainError('local Laplacian oracle cap 36 pixels')
    gauss=pyramid(image,levels,mode='gaussian',dtype=dtype);out=pyramid(image,levels,mode='laplacian',dtype=dtype)
    gs={v['level']:v['values'] for v in gauss['bands']}
    for band in out['bands']:
        if band['role']=='base':continue
        l=band['level'];hh,ww=shape(band['values'])
        for y in range(hh):
            for x in range(ww):
                g=frac(gs[l][y][x]);remapped=zeros(h,w)
                for j in range(h):
                    for i in range(w):
                        d=frac(image[j][i])-g;z=abs(d);sign=1 if d>=0 else -1
                        if not z:value=rn(g,dtype)
                        elif z>s:value=rn(g+sign*(s+b*(z-s)),dtype)
                        elif a.denominator==1:value=rn(g+sign*s*(z/s)**a.numerator,dtype)
                        else:value=certify(lambda c:c.q(g)+sign*s*c.q(z/s).pow_rational(a),dtype)
                        remapped[j][i]=value
                local=pyramid(remapped,levels,mode='laplacian',dtype=dtype)
                source=next(v for v in local['bands'] if v['level']==l and v['role']=='detail')
                band['values'][y][x]=source['values'][y][x]
    return pyramid(mode='reconstruct',bands=out,dtype=dtype)
