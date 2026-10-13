from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.fil01.spatial import convolve2d

def gradient(image,method='sobel',*,dx=1,dy=1,normalization='unit_ramp',boundary='reflect_half',cval=0,dtype='float64'):
    dx,dy=frac(dx),frac(dy)
    if min(dx,dy)<=0:raise DomainError('spacing')
    if method=='central':smooth=[0,1,0];den=2
    elif method=='sobel':smooth=[1,2,1];den=8
    elif method=='scharr':smooth=[3,10,3];den=32
    else:raise DomainError('gradient method')
    if normalization=='raw':den=1
    elif normalization!='unit_ramp':raise DomainError('gradient normalization')
    kx=[[Q(s*d,den)/dx for d in [-1,0,1]] for s in smooth]
    ky=[[Q(smooth[x]*[-1,0,1][y],den)/dy for x in range(3)] for y in range(3)]
    kw=dict(direction='correlate',boundary=boundary,cval=cval,dtype=dtype)
    return convolve2d(image,kx,(1,1),**kw),convolve2d(image,ky,(1,1),**kw)
