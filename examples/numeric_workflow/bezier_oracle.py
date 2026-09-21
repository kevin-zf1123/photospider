"""Independent Fraction de Casteljau, rational Euclid/Sturm Bezier reference."""
import math
import random
import subprocess
import sys
from fractions import Fraction as F
from comparison_oracle import number
from sequence_oracle import ieee_round


def finite(value):
    return value is not None and value not in (math.inf,-math.inf)


def rounded(value,width=64,negative_zero=False):
    bits=ieee_round(value,width,negative_zero)
    if bits is None:
        raise OverflowError
    return bits


def casteljau(control,t):
    a=list(control)
    while len(a)>1:
        a=[(1-t)*x+t*y for x,y in zip(a,a[1:])]
    return a[0]


def split(control,t):
    a=list(control);left=[a[0]];right=[a[-1]]
    while len(a)>1:
        a=[(1-t)*x+t*y for x,y in zip(a,a[1:])]
        left.append(a[0]);right.append(a[-1])
    return left,list(reversed(right))


def coefficients(control):
    d=len(control)-1;out=[F() for _ in control]
    for i,v in enumerate(control):
        for j in range(d-i+1):
            out[i+j]+=v*math.comb(d,i)*math.comb(d-i,j)*((-1)**j)
    return trim(out)


def trim(a):
    a=list(a)
    while a and not a[-1]:a.pop()
    return a


def remainder(a,b):
    a=trim(a)
    while len(a)>=len(b):
        shift=len(a)-len(b);factor=a[-1]/b[-1]
        for i,v in enumerate(b):a[i+shift]-=factor*v
        a=trim(a)
    return a


def sign_changes(values):
    signs=[v>0 for v in values if v]
    return sum(a!=b for a,b in zip(signs,signs[1:]))


def common_unit_root(f,g):
    a,b=trim(f),trim(g)
    while b:a,b=b,remainder(a,b)
    if len(a)<=1:return False
    sequence=[a,[i*v for i,v in enumerate(a)][1:]]
    while sequence[-1]:
        r=remainder(sequence[-2],sequence[-1])
        if not r:break
        sequence.append([-v for v in r])
    return sign_changes([p[0] for p in sequence])-sign_changes([sum(p) for p in sequence])>0


def boundary(low,high,dtype):
    a,b=number(low,dtype),number(high,dtype)
    if finite(a) and finite(b):return (a+b)/2
    threshold=F(2)**(128 if dtype==4 else 1024)-F(2)**(103 if dtype==4 else 970)
    return -threshold if a<0 else threshold


def order_key(bits,dtype):
    sign=1<<(31 if dtype==4 else 63);mag=bits&(sign-1)
    return sign-mag if bits&sign else sign+mag


def inverse(x,y,q,dtype):
    width=32 if dtype==4 else 64
    f=coefficients(x);f[0]-=q;g=coefficients(y)
    if not g:return 0
    if len(g)==1:return rounded(g[0],width)
    if common_unit_root(f,g):return 0
    lo,hi=F(),F(1);checked=set()
    for iteration in range(1,8193):
        mid=(lo+hi)/2;xm=casteljau(x,mid)
        if xm==q:return rounded(casteljau(y,mid),width)
        if xm<q:lo=mid
        else:hi=mid
        if iteration%8:continue
        _,right=split(y,lo)
        control,_=split(right,(hi-lo)/(1-lo))
        low,high=min(control),max(control)
        def extended(v):
            bits=ieee_round(v,width,False)
            if bits is None:bits=(0x7f800000 if dtype==4 else 0x7ff0000000000000)|(int(v<0)<<(width-1))
            return bits
        a,b=extended(low),extended(high)
        if a==b:
            if not finite(number(a,dtype)):raise OverflowError
            return a
        if order_key(b,dtype)-order_key(a,dtype)==1:
            r=boundary(a,b,dtype)
            if r not in checked:
                checked.add(r);difference=list(g);difference[0]-=r
                if common_unit_root(f,difference):return rounded(r,width)
    raise RuntimeError('unresolved independent Bezier oracle')


def reference(degree,count,k,dtype,policy,indices,ports):
    if indices and count == 1048576:
        return "resource"  # The public oracle runner admits only 1 MiB payload.
    observed = indices
    indices = list(range(count)) if indices else []
    types=[p[0] for p in ports];raw=[p[1] for p in ports]
    source=[[number(v,t) for v in r] for t,r in ports]
    a,b=source[2][0],source[3][0]
    if not finite(a) or (count>1 and not finite(b)):return 'domain'
    if count==1:
        start_bits=rounded(a,negative_zero=bool(raw[2][0]>>(31 if types[2]==4 else 63)))
        axis=[start_bits,start_bits,0];b=a
    else:
        if a==b:return 'domain'
        try:step=rounded((b-a)/(count-1))
        except OverflowError:return 'overflow'
        if not number(step,3):return 'overflow'
        axis=[rounded(a,negative_zero=bool(raw[2][0]>>(31 if types[2]==4 else 63))),
              rounded(b,negative_zero=bool(raw[3][0]>>(31 if types[3]==4 else 63))),step]
    anchors=[source[0][2*i:2*i+2] for i in range(k)]
    curves=[]
    try:
        if indices:
            if any(not finite(p[0]) for p in anchors) or any(p[0]>=q[0] for p,q in zip(anchors,anchors[1:])):return 'domain'
            for j in range(k-1):
                x=[anchors[j][0]]
                for h in range(degree-1):
                    offset=source[1][(j*(degree-1)+h)*2]
                    if not finite(offset):return 'domain'
                    x.append(number(rounded(anchors[j+(h>0)][0]+offset),3))
                x.append(anchors[j+1][0]);u=x[1]-x[0];v=x[2]-x[1]
                if degree==2:
                    if min(u,v)<0:return 'domain'
                else:
                    w=x[3]-x[2];aa=u-2*v+w;bb=2*(v-u)
                    if min(u,w)<0:return 'domain'
                    if aa>0 and 0 < -bb/(2*aa) < 1 and u-bb*bb/(4*aa)<0:return 'domain'
                curves.append(x)
        def coordinate(i):
            if not i:return a
            if i+1==count:return b
            return number(rounded(((count-1-i)*a+i*b)/(count-1)),3)
        selected=[]
        for i in indices:
            q=coordinate(i)
            if (i and coordinate(i-1)==q) or (i+1<count and coordinate(i+1)==q):return 'overflow'
            hit=next((j for j,p in enumerate(anchors) if p[0]==q),None)
            if q<anchors[0][0] or q>anchors[-1][0]:
                if not policy:return 'domain'
                hit=0 if q<anchors[0][0] else k-1
            selected.append((q,hit))
        values=[]
        for q,hit in selected:
            if hit is not None:
                yy=anchors[hit][1]
                if not finite(yy):return 'domain'
                negative_zero=not yy and bool(raw[0][2*hit+1]>>(31 if types[0]==4 else 63))
                values.append(rounded(yy,32 if dtype==4 else 64,negative_zero));continue
            j=next(j for j in range(k-1) if anchors[j][0]<q<anchors[j+1][0])
            if not finite(anchors[j][1]) or not finite(anchors[j+1][1]):return 'domain'
            y=[anchors[j][1]]
            for h in range(degree-1):
                offset=source[1][(j*(degree-1)+h)*2+1]
                if not finite(offset):return 'domain'
                y.append(number(rounded(anchors[j+(h>0)][1]+offset),3))
            y.append(anchors[j+1][1]);values.append(inverse(curves[j],y,q,dtype))
        values = [values[i] for i in observed]
        return ' '.join(f'{v:x}' for v in values)+(' ' if values else '')+'| '+' '.join(f'{v:x}' for v in axis)+' '
    except OverflowError:return 'overflow'


def cases():
    rng=random.Random(20204)
    enc=lambda values,t:[rounded(F(v),32 if t==4 else 64) for v in values]
    for degree in (2,3):
        for dtype in (3,4):
            for trial in range(80):
                k=rng.randrange(2,5);count=rng.choice((1,3,9,17));types=[rng.choice((3,4)) for _ in range(4)]
                anchors=[];handles=[]
                for j in range(k):anchors.extend((j,F(rng.randrange(-16,17),4)))
                for j in range(k-1):
                    handles.extend((F(rng.randrange(0,5),4),F(rng.randrange(-16,17),4)))
                    if degree==3:handles.extend((-F(rng.randrange(0,5),4),F(rng.randrange(-16,17),4)))
                start,end=(0,k-1) if trial%2 else (k-1,0)
                if trial%7==0:start=F(-1,4)
                if trial%9==0:start=F(1,2)
                indices=sorted({0,count//2,count-1}) if trial%11 else []
                yield degree,count,k,dtype,trial%2,indices,list(zip(types,[enc(v,t) for v,t in zip((anchors,handles,[start],[end]),types)]))
    for degree in (2,3):
        for dtype in (3,4):
            ulp=F(1,1<<(23 if dtype==4 else 52))
            for anchors,handles,start in [([0,1,1,1+ulp],[0,0] if degree==2 else [0,0,-1,-ulp],F(1,2)),
                                          ([0,-F(1,2),1,F(1,2)],[0,0] if degree==2 else [0,0,-1,-1],F(1,2))]:
                yield degree,1,2,dtype,0,[0],[(3,enc(v,3)) for v in (anchors,handles,[start],[0])]
    for sign in (-1,1):
        delta=F(sign,1<<1074)
        yield 3,1,2,3,0,[0],[(3,enc(v,3)) for v in ([0,-17*delta,45,742*delta],[4,66*delta,-38,-644*delta],[1],[0])]


    # Distinct RN64 control reconstruction boundaries and finite-extreme cases.
    special=[
        ([0,1,1,1+F(1,1<<51)],[F(1,2),F(1,1<<53)],F(1,2)),
        ([1,0,1+F(1,1<<51),1],[F(1,1<<53),0],1+F(1,1<<52)),
        ([0,0,F(4,1<<1074),0],[0,1],F(1,1<<1074)),
        ([F(1<<1023),0,number(0x7fefffffffffffff,3),1],[number(0x7fefffffffffffff,3),0],0),
        ([0,number(0x7fefffffffffffff,3),1,number(0x7fefffffffffffff,3)],[F(1,2),number(0x7fefffffffffffff,3)],F(1,2)),
        ([0,number(0x7fefffffffffffff,3),1,number(0x7fefffffffffffff,3)],[F(1,2),number(0x7fefffffffffffff,3)],0),
    ]
    for dtype in (3,4):
        for anchors,handles,start in special:
            yield 2,1,2,dtype,0,[0],[(3,enc(v,3)) for v in (anchors,handles,[start],[0])]
        for descending in (False,True):
            start,end=(1,0) if descending else (0,1)
            yield 2,1048576,2,dtype,0,[0,1,524288,1048575],[(3,enc(v,3)) for v in ([0,0,1,1],[F(1,2),F(1,2)],[start],[end])]
    for indices in ([],[0],[1],[2]):
        yield 2,3,2,3,0,indices,[(3,[rounded(F(1)),0,rounded(F(1))+1,rounded(F(1))]),(3,[0,0]),(3,[rounded(F(1))]),(3,[rounded(F(1))+1])]
    nan=0x7ff0000000000042
    for port in (0,1,2,3):
        for position in range(4 if port==0 else 2 if port==1 else 1):
            for indices in ([],[0],[1],[2]):
                raw=[enc([0,0,1,1],3),enc([F(1,2),F(1,2)],3),enc([0],3),enc([1],3)]
                raw[port][position]=nan
                yield 2,3,2,3,0,indices,[(3,v) for v in raw]
    for sign in (0,1):
        yield 2,1,2,3,0,[0],[(3,[0,sign<<63,rounded(F(1)),sign<<63]),(3,[rounded(F(1,2)),sign<<63]),(3,[sign<<63]),(3,[nan])]
        yield 2,1,2,3,0,[0],[(3,[0,sign<<63,rounded(F(1)),sign<<63]),(3,[rounded(F(1,2)),sign<<63]),(3,[rounded(F(1,2))]),(3,[nan])]


def main():
    rows=list(cases());encoded=[];wanted=[]
    for degree,count,k,dtype,policy,indices,ports in rows:
        text=' '.join(map(str,[degree,count,k,dtype,policy,len(indices),*indices]))
        for t,raw in ports:text+=f' {t} '+' '.join(f'{v:x}' for v in raw)
        encoded.append(text);wanted.append(reference(degree,count,k,dtype,policy,indices,ports))
    profile=sys.argv[2] if len(sys.argv)>2 else 'strict'
    result=subprocess.run([sys.argv[1],profile,'oracle'],input='\n'.join(encoded)+'\n',text=True,capture_output=True,check=True)
    actual=result.stdout.splitlines();assert len(actual)==len(wanted),(len(actual),len(wanted),result.stderr)
    for i,(got,expected) in enumerate(zip(actual,wanted)):
        assert got==expected,(i,rows[i],got,expected,result.stderr[:1000])
    print(f'{len(rows)-4} independent Fraction/de Casteljau/Euclid-Sturm Bezier cases and 4 full-output budget cases passed ({profile})')


if __name__=='__main__':main()
