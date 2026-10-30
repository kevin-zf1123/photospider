"""Exact small-grid KEY references; no NumPy/SciPy or production implementation.
Dense solvers intentionally cap systems at 64 unknowns; see README coverage.
"""
from __future__ import annotations
from collections import deque
from .exact import F, OracleError, frac, fq, q, rn, coverage, smooth_exact, clamp, sqrt_affine, solve_exact, inverse3
from .blend import rgba


def shape(grid):
    h=len(grid)
    if h==0 or len(grid[0])==0: raise OracleError('positive extents required')
    w=len(grid[0])
    if any(len(row)!=w for row in grid): raise OracleError('ragged image')
    return h,w


def scalar_grid(grid,dtype='f64',unit=False,binary=False):
    shape(grid)
    out=[]
    for row in grid:
        r=[]
        for x in row:
            z=coverage(x,dtype) if unit or binary else fq(x,dtype)
            if binary and z not in (0,1): raise OracleError('binary mask required')
            r.append(z)
        out.append(r)
    return out


def color_grid(grid,dtype='f64',channels=3):
    shape(grid)
    if any(len(v)!=channels for row in grid for v in row): raise OracleError('color channel mismatch')
    return [[[q(v,dtype) for v in pixel] for pixel in row] for row in grid]


def check_shape(*grids):
    s=shape(grids[0])
    if any(shape(g)!=s for g in grids[1:]): raise OracleError('grid mismatch')
    return s


def neighbours(y,x,h,w):
    return [(yy,xx) for yy,xx in ((y-1,x),(y,x-1),(y,x+1),(y+1,x)) if 0<=yy<h and 0<=xx<w]


def window(y,x,h,w,radius):
    return [(yy,xx) for yy in range(max(0,y-radius),min(h,y+radius+1))
            for xx in range(max(0,x-radius),min(w,x+radius+1))]


def radius_check(r):
    if type(r) is not int or not 0<=r<=4096: raise OracleError('radius must be Int64 in 0..4096')


def range_select(value,low,high,softness=0,invert=False,dtype='f64'):
    v,l,h,w=[fq(x,dtype) for x in (value,low,high,softness)]
    if not l<h or w<0: raise OracleError('invalid range/softness')
    out=F(int(l<=v<=h)) if w==0 else smooth_exact(l-w,l,v)*(1-smooth_exact(h,h+w,v))
    return rn(1-out if invert else out,dtype)


def lab76_key(color,key,inner,outer,dtype='f64'):
    if len(color)!=3 or len(key)!=3: raise OracleError('Lab triplet required')
    a,b=[fq(x,dtype) for x in color],[fq(x,dtype) for x in key]
    lo,hi=fq(inner,dtype),fq(outer,dtype)
    if not 0<=lo<hi: raise OracleError('invalid Lab76 thresholds')
    # Stored Lab l=L*/100, not conventional unnormalized L*.
    d2=(100*(a[0]-b[0]))**2+(a[1]-b[1])**2+(a[2]-b[2])**2
    d=frac(sqrt_affine(0,1,d2,dtype)) # explicit distance-node rounding
    return rn(smooth_exact(lo,hi,d),dtype)


def lab_chroma_key(color,key,inner,outer,dtype='f64'):
    if len(color)!=3 or len(key)!=3:raise OracleError('Lab triplet required')
    a,b=[fq(x,dtype) for x in color[1:]],[fq(x,dtype) for x in key[1:]]
    lo,hi=fq(inner,dtype),fq(outer,dtype)
    if not 0<=lo<hi:raise OracleError('invalid chroma thresholds')
    distance=frac(sqrt_affine(0,1,sum((x-y)**2 for x,y in zip(a,b)),dtype))
    return rn(smooth_exact(lo,hi,distance),dtype)


def difference_key(color,plate,inner,outer,dtype='f64'):
    if len(color)!=len(plate) or len(color)!=3: raise OracleError('RGB triplet required')
    a,b=[fq(x,dtype) for x in color],[fq(x,dtype) for x in plate]
    lo,hi=fq(inner,dtype),fq(outer,dtype)
    if not 0<=lo<hi: raise OracleError('invalid distance thresholds')
    d=frac(sqrt_affine(0,1,sum((x-y)**2 for x,y in zip(a,b)),dtype))
    return rn(smooth_exact(lo,hi,d),dtype)


def clean_plate_nearest(image,known,dtype='f64'):
    h,w=check_shape(image,known); image=color_grid(image,dtype); known=scalar_grid(known,dtype,binary=True)
    candidates=[(y,x) for y in range(h) for x in range(w) if known[y][x]]
    if not candidates: raise OracleError('InvalidDomain: no background evidence')
    result=[]; confidence=[]
    for y in range(h):
        row=[]; cr=[]
        for x in range(w):
            yy,xx=min(candidates,key=lambda p:((p[0]-y)**2+(p[1]-x)**2,p))
            row.append(image[yy][xx][:]); cr.append(float(known[y][x]))
        result.append(row); confidence.append(cr)
    return result,confidence


def temporal_median(images,valid,dtype='f64'):
    if not images or len(images)!=len(valid): raise OracleError('nonempty matching time axis required')
    h,w=check_shape(*(images+valid)); n=len(images)
    images=[color_grid(im,dtype) for im in images]; valid=[scalar_grid(v,dtype,binary=True) for v in valid]
    out=[[[0.0]*3 for _ in range(w)] for _ in range(h)]; conf=[[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            ids=[i for i in range(n) if valid[i][y][x]]
            if not ids: raise OracleError('InvalidDomain: no temporal evidence at pixel')
            for c in range(3):
                vs=sorted(((images[i][y][x][c],i) for i in ids),key=lambda v:(v[0],v[1]))
                m=len(vs)
                out[y][x][c]=vs[m//2][0] if m%2 else rn((frac(vs[m//2-1][0])+frac(vs[m//2][0]))/2,dtype)
            conf[y][x]=rn(F(len(ids),n),dtype)
    return out,conf


def foreground_regularized(image,alpha,background,prior,regularization,dtype='f64'):
    if len(image)!=3 or len(background)!=3 or len(prior)!=3: raise OracleError('RGB triplets required')
    i,b,p=([fq(x,dtype) for x in v] for v in (image,background,prior))
    a=coverage(alpha,dtype); lam=fq(regularization,dtype)
    if lam<=0: raise OracleError('positive regularization required')
    if a==0: return [q(x,dtype) for x in prior]
    if a==1: return [q(x,dtype) for x in image]
    return [rn((a*(x-(1-a)*y)+lam*z)/(a*a+lam),dtype) for x,y,z in zip(i,b,p)]


def morphology(matte,radius,mode,dtype='f64'):
    radius_check(radius); h,w=shape(matte); scalar_grid(matte,dtype,unit=True)
    a=[[q(x,dtype) for x in row] for row in matte]
    if mode not in ('erode','dilate'): raise OracleError('unknown morphology')
    fn=min if mode=='erode' else max
    # Stable extrema preserve the first row-major sample on numerical ties.
    return [[fn((a[yy][xx] for yy,xx in window(y,x,h,w,radius))) for x in range(w)] for y in range(h)]


def fill_binary_holes(mask):
    a=scalar_grid(mask,binary=True); h,w=shape(a)
    seen=set(); queue=deque()
    for y in range(h):
        for x in range(w):
            if (y in (0,h-1) or x in (0,w-1)) and a[y][x]==0:
                seen.add((y,x));queue.append((y,x))
    while queue:
        y,x=queue.popleft()
        for p in neighbours(y,x,h,w):
            if p not in seen and a[p[0]][p[1]]==0:
                seen.add(p);queue.append(p)
    return [[int(a[y][x]!=0 or (y,x) not in seen) for x in range(w)] for y in range(h)]


def guided_refine_gray(matte,guide,radius,epsilon,dtype='f64'):
    radius_check(radius); h,w=check_shape(matte,guide)
    p=scalar_grid(matte,dtype,unit=True); g=scalar_grid(guide,dtype); eps=fq(epsilon,dtype)
    if eps<=0: raise OracleError('positive epsilon required')
    if radius==0: return [[q(x,dtype) for x in row] for row in matte]
    coef={}
    for y in range(h):
        for x in range(w):
            ws=window(y,x,h,w,radius); n=len(ws)
            mg=sum(g[yy][xx] for yy,xx in ws)/n; mp=sum(p[yy][xx] for yy,xx in ws)/n
            var=sum(g[yy][xx]**2 for yy,xx in ws)/n-mg*mg
            cov=sum(g[yy][xx]*p[yy][xx] for yy,xx in ws)/n-mg*mp
            a=cov/(var+eps);coef[y,x]=(a,mp-a*mg)
    out=[]
    for y in range(h):
        row=[]
        for x in range(w):
            ws=window(y,x,h,w,radius)
            z=sum(coef[yy,xx][0]*g[y][x]+coef[yy,xx][1] for yy,xx in ws)/len(ws)
            row.append(rn(clamp(z),dtype))
        out.append(row)
    return out


def green_excess(foreground,mask,amount=0,dtype='f64'):
    v=rgba(foreground,dtype) if len(foreground)==4 else [q(x,dtype) for x in foreground]
    if len(v) not in (3,4): raise OracleError('RGB or RGBA required')
    m,k=coverage(mask,dtype),coverage(amount,dtype)
    r,g,b=map(frac,v[:3]); e=max(F(0),g-max(r,b))
    if m*k==0: return v
    return [v[0],rn(g-k*m*e,dtype),v[2]]+v[3:]


def directional_despill(foreground,spill,mask,amount,y_weights,dtype='f64'):
    v=rgba(foreground,dtype) if len(foreground)==4 else [q(x,dtype) for x in foreground]
    if len(v) not in (3,4) or len(spill)!=3 or len(y_weights)!=3: raise OracleError('RGB vectors required')
    s=[fq(x,dtype) for x in spill]; raw=[fq(x,'f64') for x in y_weights]
    if sum(raw)<=0:raise OracleError('positive linear-Y normalization required')
    w=[x/sum(raw) for x in raw]
    m,k=coverage(mask,dtype),coverage(amount,dtype)
    lum=sum(x*y for x,y in zip(s,w)); d=[x-lum for x in s]; den=sum(x*x for x in d)
    if den==0: raise OracleError('InvalidDomain: achromatic spill direction')
    if m*k==0: return v
    foreground_luma=sum(x*frac(y) for x,y in zip(w,v[:3]))
    chroma=[frac(y)-foreground_luma for y in v[:3]]
    e=max(F(0),sum(x*y for x,y in zip(d,chroma))/den)
    return [rn(frac(x)-m*k*e*y,dtype) for x,y in zip(v[:3],d)]+v[3:]


def edge_extend_nearest(image,radius,opaque_threshold=1,dtype='f64'):
    radius_check(radius); h,w=shape(image)
    if any(len(p)!=4 for row in image for p in row): raise OracleError('RGBA edge image required')
    v=[[rgba(p,dtype) for p in row] for row in image]
    threshold=coverage(opaque_threshold,dtype)
    if threshold==0: raise OracleError('positive donor-alpha threshold required')
    out=[]
    for y in range(h):
        row=[]
        for x in range(w):
            pixel=v[y][x]
            if frac(pixel[-1])>=threshold: row.append(pixel[:]);continue
            donors=[p for p in window(y,x,h,w,radius)
                    if (p[0]-y)**2+(p[1]-x)**2<=radius*radius and frac(v[p[0]][p[1]][-1])>=threshold]
            if not donors: row.append(pixel[:]);continue
            yy,xx=min(donors,key=lambda p:((p[0]-y)**2+(p[1]-x)**2,p))
            row.append(v[yy][xx][:-1]+[pixel[-1]])
        out.append(row)
    return out


def matting_laplacian(image,epsilon,dtype='f64'):
    im=color_grid(image,dtype); h,w=shape(im); n=h*w
    if n>64: raise OracleError('OracleLimit: at most 64 pixels')
    eps=fq(epsilon,dtype)
    if eps<=0: raise OracleError('positive epsilon required')
    lap=[[F(0)]*n for _ in range(n)]
    for y in range(h):
        for x in range(w):
            win=window(y,x,h,w,1); size=len(win)
            vectors=[[frac(c) for c in im[yy][xx]] for yy,xx in win]
            mu=[sum(v[c] for v in vectors)/size for c in range(3)]
            diff=[[v[c]-mu[c] for c in range(3)] for v in vectors]
            cov=[[sum(v[a]*v[b] for v in diff)/size + (eps/size if a==b else 0)
                  for b in range(3)] for a in range(3)]
            inv=inverse3(cov)
            for i,(yi,xi) in enumerate(win):
                for j,(yj,xj) in enumerate(win):
                    d=sum(diff[i][a]*inv[a][b]*diff[j][b] for a in range(3) for b in range(3))
                    lap[yi*w+xi][yj*w+xj] += F(int(i==j))-(1+d)/size
    return lap


def closed_form_matting(image,trimap,epsilon,dtype='f64'):
    h,w=check_shape(image,trimap)
    if any(type(v) is not int or v not in (0,1,2) for row in trimap for v in row):
        raise OracleError('trimap must be integer labels BG=0, unknown=1, FG=2')
    lap=matting_laplacian(image,epsilon,dtype)
    labels=[v for row in trimap for v in row]
    unknown=[i for i,v in enumerate(labels) if v==1]; known=[i for i,v in enumerate(labels) if v!=1]
    if not known: raise OracleError('InvalidDomain: no trimap anchor')
    raw=[F(int(v==2)) for v in labels]
    if unknown:
        a=[[lap[i][j] for j in unknown] for i in unknown]
        rhs=[-sum(lap[i][j]*raw[j] for j in known) for i in unknown]
        sol=solve_exact(a,rhs)
        for i,v in zip(unknown,sol): raw[i]=v
    result=[[rn(clamp(raw[y*w+x]),dtype) for x in range(w)] for y in range(h)]
    residual=[sum(lap[i][j]*raw[j] for j in range(h*w)) for i in unknown]
    return result,{'raw_solution':raw,'exact_raw_residual':residual,
                   'clipped_count':sum(v<0 or v>1 for v in raw)}
