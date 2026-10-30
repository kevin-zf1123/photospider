"""Strict references for complete grade entries and additional member behavior."""
from .precision import F, frac, rn, finite_copy, exp2, log2, sqrt, smooth, compare
from . import color, point, curves, statistics, spatial, dither


def brightness_curve(rgb,knots,ordinates,route='ratio',method='linear',edge='reject',context=None,dtype=64):
    ctx=context or color.Context()
    source=ctx.feature(rgb,route)
    target=getattr(curves,method)(source,knots,ordinates,edge,64)
    return ctx.replace(rgb,target,route,dtype)


def gradient_image(rgb,knots,colors,feature_source='linear_y',edge='clamp',context=None,dtype=64):
    ctx=context or color.Context()
    feature=max(rgb) if feature_source=='max_rgb' else ctx.feature(rgb,{'linear_y':'ratio','cielab_l':'cielab_l','oklab_l':'oklab_l'}[feature_source])
    return curves.ramp(feature,knots,colors,edge,dtype)


def neutral_region(lab,controls,dtype=64):
    l,a,b=map(frac,lab);ci,co,l0,l1,l2,l3=map(frac,controls)
    if not 0<=ci<co or not l0<l1<=l2<l3:raise ValueError('neutral controls')
    c=sqrt(a*a+b*b)
    return rn((1-smooth((c-ci)/(co-ci)))*smooth((l-l0)/(l1-l0))*(1-smooth((l-l2)/(l3-l2))),dtype)


def local(image,controls=(-2,1,0,0),edges=(-8,-4,-1,2),detail_gain=1,context=None,dtype=64,**base_parameters):
    ctx=context or color.Context()
    if frac(detail_gain)<0:raise ValueError('detail gain')
    if list(controls)[1:]==[1,0,0] and detail_gain==1:
        return [[[finite_copy(c,dtype) for c in rgb] for rgb in row] for row in image]
    feature=[[ctx.feature(rgb,'ratio') for rgb in row] for row in image]
    epsilon=base_parameters.get('epsilon',2**-16)
    out=[]
    p,c,se,he=map(frac,controls)
    for y,row in enumerate(image):
        target=[]
        for x,rgb in enumerate(row):
            b=frac(spatial.bilateral_base(feature,x,y,**base_parameters))
            z=frac(rn(log2(max(frac(feature[y][x]),frac(epsilon))),64))
            ws,wm,wh=point.zone_exact(b,edges)
            ev=(c-1)*(b-p)+se*ws+he*wh+(frac(detail_gain)-1)*(z-b)
            target.append([rn(frac(v)*exp2(ev),dtype) for v in rgb])
        out.append(target)
    return out


def metering(image,selection,roi=None,mode='binary',alpha=None,alpha_policy='exclude_zero'):
    h=len(image);w=len(image[0])
    if len(selection)!=h or any(len(r)!=w for r in selection):raise ValueError('selection shape')
    x0,y0,x1,y1=roi or (0,0,w,h)
    if not 0<=x0<=x1<=w or not 0<=y0<=y1<=h:raise ValueError('ROI')
    if alpha_policy not in ('include','exclude_zero','weight') or (alpha_policy=='weight' and mode!='weighted'):raise ValueError('alpha policy')
    mask=[[0]*w for _ in range(h)]
    for y in range(y0,y1):
        for x in range(x0,x1):
            s=frac(selection[y][x]);a=F(1) if alpha is None or alpha_policy=='include' else frac(alpha[y][x])
            if not 0<=s<=1 or not 0<=a<=1 or (mode=='binary' and s not in (0,1)):raise ValueError('mask/alpha domain')
            if alpha_policy=='exclude_zero' and a==0:s=F(0)
            if alpha_policy=='weight':s=frac(rn(s*a,64))
            mask[y][x]=int(s) if mode=='binary' else rn(s,64)
    return mask


def auto_exposure(image,selection,percentile=.5,target=.18,ev_min=-16,ev_max=16,context=None,roi=None,mode='binary',alpha=None,alpha_policy='exclude_zero',dtype=64):
    ctx=context or color.Context()
    mask=metering(image,selection,roi,mode,alpha,alpha_policy)
    feature=[[ctx.feature(v,'ratio') if mask[y][x] else 0 for x,v in enumerate(row)] for y,row in enumerate(image)]
    q,n,m=statistics.quantiles(feature,mask,[percentile],mode)
    ev,state=statistics.exposure_parameters(q[0],n,target,ev_min,ev_max)
    return [[[finite_copy(v,dtype) if state else point.exposure(v,ev,dtype) for v in rgb] for rgb in row] for row in image]


def auto_levels(image,selection,route='ratio',probabilities=(.01,.99),gamma=1,out0=0,out1=1,context=None,roi=None,mode='binary',alpha=None,alpha_policy='exclude_zero',dtype=64):
    ctx=context or color.Context()
    mask=metering(image,selection,roi,mode,alpha,alpha_policy)
    if route=='channels':
        controls=[]
        for c in range(len(image[0][0])):
            q,n,m=statistics.quantiles([[v[c] for v in row] for row in image],mask,probabilities,mode)
            controls.append(statistics.levels_parameters(q,n,gamma,out0,out1))
        return [[[finite_copy(v,dtype) if controls[c][1] else point.levels(v,*controls[c][0],dtype=dtype) for c,v in enumerate(rgb)] for rgb in row] for row in image]
    feature=[[ctx.feature(v,route) if mask[y][x] else 0 for x,v in enumerate(row)] for y,row in enumerate(image)]
    q,n,m=statistics.quantiles(feature,mask,probabilities,mode)
    controls,state=statistics.levels_parameters(q,n,gamma,out0,out1)
    if state:return [[[finite_copy(v,dtype) for v in rgb] for rgb in row] for row in image]
    return [[ctx.replace(rgb,point.levels(ctx.feature(rgb,route),*controls,dtype=64),route,dtype) for rgb in row] for row in image]


def equalize(feature,selection,bins=256,edge='clamp',mode='binary',roi=None,dtype=64):
    mask=metering(feature,selection,roi,mode,alpha_policy='include')
    counts,n,mass=statistics.histogram(feature,mask,bins,edge,mode)
    mapping,identity=statistics.cdf(counts,mode)
    return [[statistics.hist_apply(v,mapping,identity,edge,dtype) for v in row] for row in feature]


def clahe(feature,selection,cell=(64,64),bins=256,clip_factor=2,edge='clamp',mode='binary',dtype=64):
    maps,flags=statistics.clahe_maps(feature,selection,cell,bins,clip_factor,edge,mode)
    shape=(len(feature),len(feature[0]))
    return [[statistics.clahe_apply(x,y,v,maps,flags,shape,cell,edge,dtype) for x,v in enumerate(row)] for y,row in enumerate(feature)]


def vignette(rgb,x,y,dtype=64,**parameters):
    ev=spatial.vignette(x,y,**parameters)
    return [point.exposure(v,ev,dtype) for v in rgb]


def halftone_cmyk(cmyk,x,y,screens,dtype=64):
    if len(cmyk)!=4 or len(screens)!=4:raise ValueError('four CMYK screens required')
    out=[]
    for v,screen in zip(cmyk,screens):
        p=dict(screen);method=p.pop('method')
        out.append(getattr(dither,'halftone_'+method)(v,x,y,dtype=dtype,**p))
    return out


def colorize(ink,colors,background,dtype=64):
    v=list(map(frac,background))
    if len(ink)!=len(colors) or len(v)!=3:raise ValueError('colorize shape')
    for coverage,rgb in zip(ink,colors):
        c=frac(coverage)
        if not 0<=c<=1:raise ValueError('coverage')
        v=[(1-c)*a+c*frac(b) for a,b in zip(v,rgb)]
    return [rn(x,dtype) for x in v]


def palette_point(rgb,palette,threshold,context=None,dtype=64):
    ctx=context or color.Context()
    x=list(map(frac,ctx.model(rgb,'oklab_l')))
    p=[list(map(frac,ctx.model(v,'oklab_l'))) for v in palette]
    best=None
    for i in range(len(p)):
        for j in range(i+1,len(p)):
            delta=[b-a for a,b in zip(p[i],p[j])];den=color.dot(delta,delta)
            if den==0:continue
            f=max(F(0),min(F(1),color.dot([a-b for a,b in zip(x,p[i])],delta)/den))
            residual=[a-(b+f*d) for a,b,d in zip(x,p[i],delta)]
            candidate=(color.dot(residual,residual),i,j,f)
            if best is None or candidate[:3]<best[:3]:best=candidate
    index=0 if best is None else best[2] if frac(threshold)<best[3] else best[1]
    return [finite_copy(c,dtype) for c in palette[index]]


def palette_ordered(rgb,palette,x,y,thresholds,origin=(0,0),context=None,dtype=64):
    t=thresholds[(y-origin[1])%len(thresholds)][(x-origin[0])%len(thresholds[0])]
    return palette_point(rgb,palette,t,context,dtype)


def palette_fs(image,palette,scan='serpentine',origin_y=0,context=None,dtype=64):
    ctx=context or color.Context();h=len(image);w=len(image[0])
    p=[list(map(frac,ctx.model(v,'oklab_l'))) for v in palette]
    errors=[[[F(0)]*3 for _ in range(w)] for _ in range(h)]
    output=[[None]*w for _ in range(h)]
    for y in range(h):
        direction=-1 if scan=='serpentine' and (origin_y+y)%2 else 1
        for x in (range(w) if direction==1 else range(w-1,-1,-1)):
            value=[frac(rn(frac(v)+e,64)) for v,e in zip(ctx.model(image[y][x],'oklab_l'),errors[y][x])]
            index=min(range(len(p)),key=lambda i:(sum((a-b)**2 for a,b in zip(value,p[i])),i))
            output[y][x]=[finite_copy(v,dtype) for v in palette[index]]
            error=[frac(rn(a-b,64)) for a,b in zip(value,p[index])]
            for dx,dy,weight in [(direction,0,F(7,16)),(-direction,1,F(3,16)),(0,1,F(5,16)),(direction,1,F(1,16))]:
                xx,yy=x+dx,y+dy
                if 0<=xx<w and yy<h:
                    errors[yy][xx]=[frac(rn(a+frac(rn(e*weight,64)),64)) for a,e in zip(errors[yy][xx],error)]
    return output
