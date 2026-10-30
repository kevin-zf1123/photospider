"""CMP/BLN/NBL independent references. Public image helpers use RGBA/GrayA
lists only as fixture adapters; production semantic channel indices are explicit.
"""
from __future__ import annotations
from math import copysign
from .exact import F, OracleError, frac, fq, q, coverage, rn, clamp, sqrt_affine

PD_MODES = ('clear','source','destination','source_over','destination_over',
            'source_in','destination_in','source_out','destination_out',
            'source_atop','destination_atop','xor','plus_raw','plus_lighter_bounded')
SEPARABLE = ('normal','add','subtract','divide','multiply','multiply_unbounded',
             'darken','lighten','screen','screen_unbounded','overlay','hard_light',
             'soft_light_w3c','color_dodge','color_burn','linear_dodge','linear_burn',
             'linear_light','vivid_light','pin_light','difference','exclusion',
             'hard_mix_vivid','hard_mix_sum')


def rgba(value, dtype='f64'):
    if len(value) not in (2,4): raise OracleError('expected GrayA or RGBA fixture')
    v = [q(x,dtype) for x in value]
    coverage(v[-1],dtype)
    return v


def pd_factors(mode, a, b):
    table = {
        'clear': (F(0),F(0)), 'source': (F(1),F(0)),
        'destination': (F(0),F(1)), 'source_over': (F(1),1-a),
        'destination_over': (1-b,F(1)), 'source_in': (b,F(0)),
        'destination_in': (F(0),a), 'source_out': (1-b,F(0)),
        'destination_out': (F(0),1-a), 'source_atop': (b,1-a),
        'destination_atop': (1-b,a), 'xor': (1-b,1-a),
        'plus_raw': (F(1),F(1)), 'plus_lighter_bounded': (F(1),F(1)),
    }
    try: return table[mode]
    except KeyError as ex: raise OracleError('unknown Porter-Duff mode') from ex


def composite(source, background, mode='source_over', dtype='f64'):
    if len(source) != len(background) or len(source) not in (2,4): raise OracleError('shape mismatch or unsupported color group')
    # Selection/clear have deliberately smaller payload demand than other modes.
    if mode == 'clear': return [0.0]*len(source)
    if mode == 'source': return rgba(source,dtype)
    if mode == 'destination': return rgba(background,dtype)
    s=rgba(source,dtype) if mode not in ('destination_in','destination_out') else [0.]*(len(source)-1)+[q(source[-1],dtype)]
    bg=rgba(background,dtype) if mode not in ('source_in','source_out') else [0.]*(len(background)-1)+[q(background[-1],dtype)]
    coverage(s[-1],dtype);coverage(bg[-1],dtype)
    a,b = frac(s[-1]),frac(bg[-1])
    fs,fb = pd_factors(mode,a,b)
    mass = fs*a+fb*b
    color = [fs*a*frac(x)+fb*b*frac(y) for x,y in zip(s[:-1],bg[:-1])]
    if mode == 'plus_raw':
        return {'color_mass':[rn(x,dtype) for x in color], 'mass':rn(mass,dtype)}
    if mode == 'plus_lighter_bounded':
        if any(not 0 <= x <= 1 for x in s[:-1]+bg[:-1]):
            raise OracleError('bounded plus requires unit-domain straight colors')
        mass = min(F(1),mass)
        color = [min(F(1),x) for x in color]
    alpha = bg[-1] if mode=='source_atop' else s[-1] if mode=='destination_atop' else rn(mass,dtype)
    if mass == 0: return [0.0]*(len(source)-1)+[alpha]
    return [rn(x/mass,dtype) for x in color]+[alpha]


def composite_blended(source, background, blended, dtype='f64'):
    s,bg=rgba(source,dtype),rgba(background,dtype)
    if len(s)!=len(bg) or len(blended)!=len(s)-1: raise OracleError('shape mismatch')
    f=[fq(x,dtype) for x in blended]
    a,b=frac(s[-1]),frac(bg[-1])
    out=a+b-a*b
    if out == 0: return [0.0]*len(s)
    c=[(1-a)*b*frac(y)+(1-b)*a*frac(x)+a*b*z
       for x,y,z in zip(s[:-1],bg[:-1],f)]
    return [rn(x/out,dtype) for x in c]+[rn(out,dtype)]


def _dodge(b,s):
    if b==0: return F(0)
    if s==1: return F(1)
    return min(F(1), b/(1-s))


def _burn(b,s):
    if b==1: return F(1)
    if s==0: return F(0)
    return 1-min(F(1),(1-b)/s)


def _vivid(b,s):
    return _burn(b,2*s) if s<=F(1,2) else _dodge(b,2*s-1)


def blend_channel(background, source, mode, dtype='f64'):
    if mode not in SEPARABLE: raise OracleError('unknown separable blend mode')
    if mode=='normal':return q(source,dtype)
    bv,sv=q(background,dtype),q(source,dtype)
    b,s=frac(bv),frac(sv)
    bounded = mode not in ('normal','add','subtract','divide','multiply_unbounded',
                           'screen_unbounded','darken','lighten','difference')
    if bounded and not (0<=b<=1 and 0<=s<=1):
        raise OracleError('InvalidDomain: bounded blend input')
    if mode=='normal': return sv
    if mode=='darken': return bv if b<=s else sv
    if mode=='lighten': return bv if b>=s else sv
    if mode=='add':
        return rn(b+s,dtype,b==s==0 and copysign(1,bv)<0 and copysign(1,sv)<0)
    if mode=='subtract':
        return rn(b-s,dtype,b==s==0 and copysign(1,bv)<0 and copysign(1,sv)>0)
    if mode=='divide':
        if s==0: raise OracleError('InvalidDomain: zero divisor')
        return rn(b/s,dtype,b==0 and copysign(1,bv)!=copysign(1,sv))
    if mode in ('multiply','multiply_unbounded'):
        return rn(b*s,dtype,b*s==0 and copysign(1,bv)!=copysign(1,sv))
    if mode in ('screen','screen_unbounded'): v=b+s-b*s
    elif mode=='overlay': v=2*b*s if b<=F(1,2) else 1-2*(1-b)*(1-s)
    elif mode=='hard_light': v=2*b*s if s<=F(1,2) else 1-2*(1-b)*(1-s)
    elif mode=='soft_light_w3c':
        if s<=F(1,2): v=b-(1-2*s)*b*(1-b)
        elif b<=F(1,4):
            d=((16*b-12)*b+4)*b
            v=b+(2*s-1)*(d-b)
        else:
            return sqrt_affine(b-(2*s-1)*b,2*s-1,b,dtype)
    elif mode=='color_dodge': v=_dodge(b,s)
    elif mode=='color_burn': v=_burn(b,s)
    elif mode=='linear_dodge': v=min(F(1),b+s)
    elif mode=='linear_burn': v=max(F(0),b+s-1)
    elif mode=='linear_light': v=clamp(b+2*s-1)
    elif mode=='vivid_light': v=_vivid(b,s)
    elif mode=='pin_light': v=min(b,2*s) if s<=F(1,2) else max(b,2*s-1)
    elif mode=='difference': v=abs(b-s)
    elif mode=='exclusion': v=b+s-2*b*s
    elif mode=='hard_mix_vivid': v=F(int(_vivid(b,s)>=F(1,2)))
    elif mode=='hard_mix_sum': v=F(int(b+s>=1))
    else: raise OracleError('unhandled mode')
    return rn(v,dtype)


def blend_color(background, source, mode, dtype='f64', y_weights=None):
    if len(background)!=len(source) or len(source) not in (1,3):
        raise OracleError('expected matching Gray or RGB colors')
    if mode.startswith(('darker_color_','lighter_color_')):
        b,s=[q(x,dtype) for x in background],[q(x,dtype) for x in source]
        if mode.endswith(('oklab_l','cielab_l')):
            if len(source)!=3:raise OracleError('native three-coordinate model required')
            take_b=b[0]<=s[0] if mode.startswith('darker') else b[0]>=s[0]
            return b if take_b else s
        if not mode.endswith('linear_y'):raise OracleError('unknown color measure')
        if len(source)!=3 or y_weights is None or len(y_weights)!=3:
            raise OracleError('explicit three-component linear-Y weights required')
        raw=[fq(x,'f64') for x in y_weights]
        if sum(raw)<=0:raise OracleError('positive Y normalization required')
        w=[x/sum(raw) for x in raw]
        yb=rn(sum(x*frac(y) for x,y in zip(w,b)),'f64')
        ys=rn(sum(x*frac(y) for x,y in zip(w,s)),'f64')
        take_b = yb<=ys if mode.startswith('darker') else yb>=ys
        return b if take_b else s
    return [blend_channel(b,s,mode,dtype) for b,s in zip(background,source)]


MASK64=(1<<64)-1

def splitmix64(value: int) -> int:
    z=(value+0x9e3779b97f4a7c15)&MASK64
    z=((z^(z>>30))*0xbf58476d1ce4e5b9)&MASK64
    z=((z^(z>>27))*0x94d049bb133111eb)&MASK64
    return (z^(z>>31))&MASK64


def counter64(seed: int, *coordinates: int) -> int:
    if type(seed) is not int or not -(1<<63)<=seed<(1<<63): raise OracleError('seed outside Int64')
    z=splitmix64(seed&MASK64)
    for c in coordinates:
        if type(c) is not int or not -(1<<63)<=c<(1<<63): raise OracleError('coordinate outside Int64')
        z=splitmix64(z^(c&MASK64))
    return z


def dissolve(source, background, seed=0, x=0, y=0, frame=0, dtype='f64'):
    s,b=rgba(source,dtype),rgba(background,dtype)
    if len(s)!=len(b): raise OracleError('shape mismatch')
    # Compare an integer against the exact threshold: no floating RNG conversion.
    take=F(counter64(seed,x,y,frame),1<<64)<frac(s[-1])
    return s[:-1]+[1.0] if take else b


def emission_add(background, emission, gain=0, dtype='f64'):
    bg=rgba(background,dtype)
    if len(bg)!=4: raise OracleError('RGB plus alpha required')
    if len(emission)!=len(bg)-1: raise OracleError('shape mismatch')
    e=[fq(x,dtype) for x in emission]; g=fq(gain,dtype)
    if g<0: raise OracleError('gain must be nonnegative')
    if g==0: return bg
    return [rn(frac(x)+g*y,dtype) for x,y in zip(bg[:-1],e)]+[bg[-1]]


def premul_mix(a,b,t,dtype='f64'):
    a,b=rgba(a,dtype),rgba(b,dtype)
    if len(a)!=len(b): raise OracleError('shape mismatch')
    t=coverage(t,dtype)
    if t==0: return a
    if t==1: return b
    aa,ab=frac(a[-1]),frac(b[-1]); alpha=(1-t)*aa+t*ab
    if alpha==0: return [0.0]*len(a)
    return [rn(((1-t)*aa*frac(x)+t*ab*frac(y))/alpha,dtype)
            for x,y in zip(a[:-1],b[:-1])]+[rn(alpha,dtype)]


def _lum(c): return F(30,100)*c[0]+F(59,100)*c[1]+F(11,100)*c[2]

def _clip_color(c):
    l,n,x=_lum(c),min(c),max(c)
    if n<0: c=[l+(v-l)*l/(l-n) for v in c]
    if x>1: c=[l+(v-l)*(1-l)/(x-l) for v in c]
    return c


def _set_lum(c,l):
    d=l-_lum(c)
    return _clip_color([v+d for v in c])


def _set_sat(c,s):
    indices=sorted(range(3),key=lambda i:(c[i],i))
    lo,mid,hi=indices
    out=[F(0)]*3
    if c[hi]>c[lo]:
        out[mid]=(c[mid]-c[lo])*s/(c[hi]-c[lo]); out[hi]=s
    return out


def nonseparable(background, source, mode, model='lab', threshold=0, dtype='f64',neutral_policy='zero_hue'):
    if len(background)!=3 or len(source)!=3: raise OracleError('three components required')
    if model not in ('lab','oklab','w3c'):raise OracleError('unknown model')
    if model!='w3c':
        if mode=='color':return [q(background[0],dtype),q(source[1],dtype),q(source[2],dtype)]
        if mode=='lightness':return [q(source[0],dtype),q(background[1],dtype),q(background[2],dtype)]
        if mode=='chroma' and neutral_policy not in ('zero_hue','preserve_gray','source_hue'):raise OracleError('neutral policy')
        # Hue and chroma formulas have no source-lightness dependency.
        source=[0,source[1],source[2]]
    b,s=[q(x,dtype) for x in background],[q(x,dtype) for x in source]
    bf,sf=list(map(frac,b)),list(map(frac,s))
    if mode not in ('hue','chroma','color','lightness'): raise OracleError('unknown nonseparable mode')
    if model=='w3c':
        if any(not 0<=x<=1 for x in bf+sf): raise OracleError('bounded W3C RGB required')
        if mode=='hue': out=_set_lum(_set_sat(sf,max(bf)-min(bf)),_lum(bf))
        elif mode=='chroma': out=_set_lum(_set_sat(bf,max(sf)-min(sf)),_lum(bf))
        elif mode=='color': out=_set_lum(sf,_lum(bf))
        else: out=_set_lum(bf,_lum(sf))
        return [rn(x,dtype) for x in out]
    if model not in ('lab','oklab'): raise OracleError('unknown model')
    e=fq(threshold,dtype)
    if e<0: raise OracleError('negative achromatic threshold')
    cb=bf[1]*bf[1]+bf[2]*bf[2]; cs=sf[1]*sf[1]+sf[2]*sf[2]
    if mode=='color': return [b[0],s[1],s[2]]
    if mode=='lightness': return [s[0],b[1],b[2]]
    if mode=='hue':
        if cs<=e*e: return b # undefined source hue: preserve background
        if cb==0: return [b[0],0.0,0.0]
        return [b[0]]+[sqrt_affine(0,v,cb/cs,dtype) for v in sf[1:]]
    if cb<=e*e:
        if neutral_policy=='preserve_gray':return b
        if neutral_policy=='source_hue':return [b[0],s[1],s[2]]
        if neutral_policy!='zero_hue':raise OracleError('unknown neutral policy')
        return [b[0],sqrt_affine(0,1,cs,dtype),0.0]
    return [b[0]]+[sqrt_affine(0,v,cs/cb,dtype) for v in bf[1:]]
