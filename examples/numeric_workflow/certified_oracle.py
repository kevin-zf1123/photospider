"""Independent NUM04/05 mathematical special tables and MPFR enclosures."""
import math,random,struct,subprocess,sys
from math_oracle_support import direct,cardinal,rational_pi
from comparison_oracle import number
from reduction_oracle import format_info,nan_convert
names=['exp','log','sin','cos','tan','sinpi','cospi','tanpi','sinc','sincpi','pow','atan2','atan2pi','sinpi','cospi','tanpi','sincpi']
def reference(kind,dtype,a,b):
    p,bias,width=format_info(dtype);sign=1<<(width-1);one=bias<<p;inf=((1<<(width-p-1))-1)<<p;nan=inf|(1<<(p-1))
    if kind>=13:
        n=a-(1<<64) if a>>63 else a
        if b==0 or b>>63:return 'invalid'
        return rational_pi(names[kind],dtype,n,b)
    x,y=number(a,dtype),number(b,dtype)
    if kind==10 and (y==0 or a==one):return one
    if x is None or (kind in (10,11,12) and y is None):return nan_convert(a if x is None else b,dtype,dtype)
    if kind in (8,9):
        if x==0:return one
        if x in (math.inf,-math.inf):return 0
        if kind==9 and x.denominator==1:return 0
        return cardinal(names[kind],dtype,a)
    if kind in (5,6,7) and x not in (math.inf,-math.inf):
        if x.denominator==1:
            if kind==6:return one|(sign if x.numerator%2 else 0)
            return a&sign
        if x.denominator==2:
            if kind==6:return 0
            if kind==7:return nan
    # Direct MPFR special NaNs are normalized to the repository's generated bits.
    result=direct(names[kind],dtype,a,b if kind in (10,11,12) else None)
    if (result&(sign-1))>inf:return nan
    return result
