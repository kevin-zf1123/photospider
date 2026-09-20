"""Independent integer/Fraction/bit oracle for exact elementary functions."""
import math,random,subprocess,sys
from fractions import Fraction
from comparison_oracle import number
from reduction_oracle import nan_convert,format_info,root_round
from sequence_oracle import ieee_round

def reference(kind,dtype,a,b):
    x,y=number(a,dtype),number(b,dtype)
    if dtype in (1,2):
        value=x
        if kind==0:value=abs(x)
        if kind==1:value=-x
        if kind==6:value=(x>0)-(x<0)
        if kind==8:value=x+y
        if kind==9:value=x-y
        if kind==10:value=x*y
        if kind==12:value=min(x,y)
        if kind==13:value=max(x,y)
        low,high=(0,255) if dtype==1 else (-(1<<63),(1<<63)-1)
        return int(value)%(1<<(8 if dtype==1 else 64)) if low<=value<=high else 'overflow'
    fraction,bias,width=format_info(dtype)
    sign=1<<(width-1);inf=((1<<(width-fraction-1))-1)<<fraction;nan=inf|(1<<(fraction-1));one=bias<<fraction
    if x is None or (kind>=8 and y is None):
        value=nan_convert(a if x is None else b,dtype,dtype)
        return value&~sign if kind==0 else value^sign if kind==1 else value
    if kind==0:return a&~sign
    if kind==1:return a^sign
    if kind==6:return (one|(a&sign)) if x else a
    if kind in (3,4,5):
        if x in (math.inf,-math.inf) or x==0:return a
        z=math.floor(x) if kind==3 else math.ceil(x) if kind==4 else round(x)
        return ieee_round(Fraction(z),width,bool(a&sign))
    if kind in (12,13):
        if x==y==0:return a|b if kind==12 else a&b
        return a if (x<y if kind==12 else x>y) else b
    if kind==2:
        if x==0:return a
        if x<0:return nan
        if x==math.inf:return inf
        return root_round(x,dtype)
    if kind==7:
        if x==0:return inf|(a&sign)
        if x in (math.inf,-math.inf):return a&sign
        exact=1/x;negative=False
    elif kind in (8,9):
        y2=-y if kind==9 else y
        if x in (math.inf,-math.inf) or y2 in (math.inf,-math.inf):
            return nan if x==-y2 else inf|(sign if (x if x in (math.inf,-math.inf) else y2)<0 else 0)
        exact=x+y2
        negative=bool(a&sign) and bool((b^sign if kind==9 else b)&sign) and not x and not y
    elif kind==10:
        if (x==0 and y in (math.inf,-math.inf)) or (y==0 and x in (math.inf,-math.inf)):return nan
        negative=bool((a^b)&sign)
        if x in (math.inf,-math.inf) or y in (math.inf,-math.inf):return inf|(sign if negative else 0)
        exact=x*y
    elif kind==11:
        negative=bool((a^b)&sign)
        if (x==y==0) or (x in (math.inf,-math.inf) and y in (math.inf,-math.inf)):return nan
        if y==0 or x in (math.inf,-math.inf):return inf|(sign if negative else 0)
        if x==0 or y in (math.inf,-math.inf):return sign if negative else 0
        exact=x/y
    rounded=ieee_round(exact,width,negative)
    return rounded if rounded is not None else inf|(sign if exact<0 else 0)
