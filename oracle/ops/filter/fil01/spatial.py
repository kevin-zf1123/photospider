from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import coordinate
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.special import negative
from oracle.ops.filter.common.special import sum_products

def convolution_geometry(h,w,kh,kw,anchor,output_shape,direction):
    ay,ax=anchor
    if not (0<=ay<kh and 0<=ax<kw):raise DomainError('anchor outside kernel')
    if direction not in ('convolve','correlate'):raise DomainError('direction')
    if output_shape=='same':return (h,w),(0,0)
    if output_shape=='full':
        return (h+kh-1,w+kw-1),((-ay,-ax) if direction=='convolve' else (ay-kh+1,ax-kw+1))
    if output_shape=='valid':
        if h<kh or w<kw:raise DomainError('nonpositive valid extent')
        return (h-kh+1,w-kw+1),((kh-1-ay,kw-1-ax) if direction=='convolve' else (ay,ax))
    raise DomainError('output shape')

def tap_position(y,x,j,i,anchor,direction):
    ay,ax=anchor
    return (y+ay-j,x+ax-i) if direction=='convolve' else (y+j-ay,x+i-ax)

def support_for_output(input_shape,kernel,anchor,q,direction='convolve',boundary='reflect_half',origin=(0,0)):
    h,w=input_shape;kh,kw=shape(kernel);out=set()
    for j in range(kh):
        for i in range(kw):
            if frac(kernel[j][i]):
                yy,xx=tap_position(q[0]+origin[0],q[1]+origin[1],j,i,anchor,direction)
                p=coordinate(yy,xx,h,w,boundary)
                if p is not None:out.add(p)
    return out

def convolve2d(image,kernel,anchor=(0,0),*,direction='convolve',normalization='none',bias=0,
               boundary='reflect_half',cval=0,output_shape='same',dtype='float64'):
    h,w=shape(image);kh,kw=shape(kernel)
    (oh,ow),origin=convolution_geometry(h,w,kh,kw,anchor,output_shape,direction)
    if boundary=='truncate':raise DomainError('ordinary convolution has no truncate')
    if output_shape!='same' and (boundary!='constant' or frac(cval)!=0):raise DomainError('full/valid require zero extension')
    k=[[frac(x) for x in row] for row in kernel]
    if normalization=='none':den=Q(1)
    elif normalization=='sum':den=sum((sum(row,Q()) for row in k),Q())
    elif normalization=='l1':den=sum((sum(map(abs,row),Q()) for row in k),Q())
    else:raise DomainError('normalization')
    if not den:raise DomainError('zero normalizer')
    bias_negative_zero = bias == 0 and negative(bias)
    bias=frac(bias);frac(cval);out=zeros(oh,ow)
    for y in range(oh):
        for x in range(ow):
            terms=[]
            for j in range(kh):
                for i in range(kw):
                    if k[j][i]:
                        yy,xx=tap_position(y+origin[0],x+origin[1],j,i,anchor,direction)
                        p=coordinate(yy,xx,h,w,boundary)
                        value=cval if p is None else image[p[0]][p[1]]
                        terms.append((k[j][i],value))
            total=sum_products(terms)
            # Special values bypass finite Fraction conversion; finite E remains exact.
            import math
            if isinstance(total,float) and not math.isfinite(total):
                out[y][x]=rn(total if math.isnan(total) else total/float(1 if den>0 else -1),dtype)
            else:
                exact=frac(total)/den+bias
                quotient_negative_zero = total == 0 and (negative(total) != (den < 0))
                out[y][x]=-0.0 if total == 0 and bias == 0 and quotient_negative_zero and bias_negative_zero else rn(exact,dtype)
    return out

def normalized_convolution(image,mask,kernel,anchor=(0,0),*,boundary='truncate',cval=0,empty='copy_center',dtype='float64'):
    h,w=same_shape(image,mask);kh,kw=shape(kernel)
    convolution_geometry(h,w,kh,kw,anchor,'same','convolve')
    k=[[frac(x) for x in row] for row in kernel]
    if any(x<0 for row in k for x in row):raise DomainError('negative positive-kernel weight')
    if empty not in ('copy_center','zero','error'):raise DomainError('empty policy')
    out=zeros(h,w);valid=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            num=den=Q()
            for j in range(kh):
                for i in range(kw):
                    weight=k[j][i]
                    if not weight:continue
                    yy,xx=tap_position(y,x,j,i,anchor,'convolve')
                    p=coordinate(yy,xx,h,w,boundary)
                    if p is None:
                        if boundary=='truncate':continue
                        m=Q(1);v=frac(cval)
                    else:
                        m=frac(mask[p[0]][p[1]])
                        if not 0<=m<=1:raise DomainError('mask range')
                        if not m:continue
                        v=frac(image[p[0]][p[1]])
                    den+=weight*m;num+=weight*m*v
            if den:out[y][x]=rn(num/den,dtype);valid[y][x]=1
            elif empty=='copy_center':out[y][x]=image[y][x]
            elif empty=='zero':out[y][x]=0.0
            else:raise DomainError('zero normalized denominator')
    return out,valid

def straight_positive_blur(colors,alphas,weights,*,dtype='float64'):
    """Single output component of the proposed fused straight/alpha formula."""
    if not(len(colors)==len(alphas)==len(weights)):raise DomainError('tap lengths')
    total=alpha=premul=Q()
    for color,a,k in zip(colors,alphas,weights):
        k=frac(k)
        if k<0:raise DomainError('negative positive-kernel weight')
        if not k:continue
        a,c=frac(a),frac(color)
        if not 0<=a<=1:raise DomainError('alpha range')
        total+=k;alpha+=k*a;premul+=k*a*c
    if total<=0:raise DomainError('empty kernel')
    return (rn(premul/alpha,dtype) if alpha else 0.0),rn(alpha/total,dtype)
