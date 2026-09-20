"""Independent Fraction grid reconstruction and single-round LUT interpolation."""
import bisect
import itertools
import math
import random
import subprocess
import sys
from fractions import Fraction as F
from comparison_oracle import number
from sequence_oracle import ieee_round


def finite(v):
    return v is not None and v not in (math.inf, -math.inf)


def rn(v, dtype=3, negative=False):
    result = ieee_round(v, 32 if dtype == 4 else 64, negative)
    if result is None:
        raise OverflowError
    return result


def reference(channels,dtype,policy,shape,l,c,coords,ports):
    if not coords:
        return ''
    rawaxis=ports[2][1]; a,b,step=[number(v,3) for v in rawaxis]
    if not all(finite(v) for v in (a,b,step)):
        return 'domain'
    if l==1:
        if rawaxis[0]!=rawaxis[1] or rawaxis[2]!=0:
            return 'domain'
        grid=[a]
    else:
        if a==b:
            return 'domain'
        try:
            expected=rn((b-a)/(l-1))
        except OverflowError:
            return 'domain'
        if not number(expected,3) or expected!=rawaxis[2]:
            return 'domain'
        grid=[a]+[number(rn(((l-1-j)*a+j*b)/(l-1)),3) for j in range(1,l-1)]+[b]
        if any((y-x)*(b-a)<=0 for x,y in zip(grid,grid[1:])):
            return 'domain'
    descending=b<a
    ordered=[-x if descending else x for x in grid]
    selections=[]
    for at in coords:
        flat=0
        for index,extent in zip(at,shape):flat=flat*extent+index
        q=number(ports[0][1][flat],ports[0][0])
        if not finite(q):return 'domain'
        key=-q if descending else q
        pos=bisect.bisect_left(ordered,key)
        if pos<l and ordered[pos]==key:
            selected=[pos]
        elif pos==0 or pos==l:
            if policy==0:return 'domain'
            selected=[0 if pos==0 else l-1] if policy==1 or l==1 else [0,1] if pos==0 else [l-2,l-1]
        else:selected=[pos-1,pos]
        selections.append((at,q,selected))
    out=[]
    try:
        for at,q,selected in selections:
            col=at[-1] if channels else 0
            raw=[ports[1][1][j*c+col] for j in selected]
            y=[number(v,ports[1][0]) for v in raw]
            if not all(finite(v) for v in y):return 'domain'
            neg=[bool(v>>(31 if ports[1][0]==4 else 63)) for v in raw]
            if len(selected)==1:
                out.append(rn(y[0],dtype,neg[0]));continue
            x0,x1=[grid[j] for j in selected]
            exact=((x1-q)*y[0]+(q-x0)*y[1])/(x1-x0)
            out.append(rn(exact,dtype,not exact and not any(y) and all(neg)))
        return ''.join(f'{v:x} ' for v in out)
    except OverflowError:
        return 'overflow'


def cases():
    rng=random.Random(20505)
    enc=lambda values,t:[rn(F(v),t) for v in values]
    for channels in (0,1):
        for dtype in (3,4):
            for trial in range(180):
                l=rng.randrange(1,18);c=rng.randrange(1,5) if channels else 1
                shape=[rng.randrange(1,6),c] if channels else [rng.randrange(1,5),rng.randrange(1,4)]
                if trial%13==0:shape=[c] if channels else [3]
                a,b=F(rng.randrange(-8,1),4),F(rng.randrange(1,9),4)
                if trial%2:a,b=b,a
                if l==1:b=a
                axis=[rn(a),rn(b),rn((b-a)/(l-1)) if l>1 else 0]
                it,tt=rng.choice((3,4)),rng.choice((3,4))
                qs=[rng.choice((a,b,(a+b)/2,min(a,b)-F(1,4),max(a,b)+F(1,4))) for _ in range(math.prod(shape))]
                table=[F(rng.randrange(-100,101),8) for _ in range(l*c)]
                coords=[at for at in itertools.product(*(range(n) for n in shape)) if trial%3 or rng.randrange(2)]
                yield channels,dtype,trial%3,shape,l,c,coords,[(it,enc(qs,it)),(tt,enc(table,tt)),(3,axis)]
    for channels in (0,1):
        for dtype in (3,4):
            for trial in range(120):
                a,b=[number(rng.getrandbits(64),3) for _ in range(2)]
                if not finite(a) or not finite(b) or a==b:continue
                step=ieee_round(b-a,64,False)
                if step is None or not number(step,3):continue
                query=rn((a+b)/2)
                table=[rng.getrandbits(64) for _ in range(2)]
                table=[v if finite(number(v,3)) else 0 for v in table]
                yield channels,dtype,0,[1],2,1,[(0,)],[(3,[query]),(3,table),(3,[rn(a),rn(b),step])]
    nan=0x7ff0000000000042;sign=1<<63
    for channels in (0,1):
        for dtype in (3,4):
            for negative in (False,True):
                for zero in (0,sign):
                    axis=[rn(F(1)),0,rn(F(-1,2))] if negative else [0,rn(F(1)),rn(F(1,2))]
                    table=[sign,zero,1]
                    if negative:table.reverse()
                    yield channels,dtype,0,[3],3,3 if channels else 1,[(0,),(1,),(2,)],[(3,enc([0,F(1,2),1],3)),(3,[v for x in table for v in ([x]*3 if channels else [x])]),(3,axis)]
                    yield channels,dtype,2,[3],3,3 if channels else 1,[(0,),(1,),(2,)],[(3,enc([F(-1,4),F(1,4),F(3,4)],3)),(3,[v for x in table for v in ([x]*3 if channels else [x])]),(3,axis)]
            for axis in ([rn(F(1)),rn(F(1))+1,rn(F(1,1<<53))], [0,rn(F(1)),rn(F(1))], [0,rn(F(1)),nan], [sign,0,0], [sign,sign,sign]):
                l=1 if axis[0]==sign else 3
                yield channels,dtype,1,[1],l,1,[(0,)],[(3,[0]),(3,[rn(F(7))]*l),(3,list(axis))]
            for badport in range(3):
                for index in range(3):
                    for selected in range(3):
                        ports=[(3,enc([0,F(1,4),1],3)),(3,enc([0,F(1,4),1],3)),(3,enc([0,1,F(1,2)],3))]
                        ports[badport][1][index]=nan
                        yield channels,dtype,0,[3] if not channels else [3,1],3,1,[(selected,)] if not channels else [(selected,0)],ports
            for a,b in [(sign,sign),(0,0),(rn(F(2)),rn(F(2)))]:
                for policy in range(3):
                    yield channels,dtype,policy,[1],1,1,[(0,)],[(3,[a]),(3,[sign]),(3,[a,b,0])]
            for coords in ([],[(0,)]):
                yield channels,dtype,0,[1],2,1,coords,[(3,[nan]),(3,[nan,nan]),(3,[nan,nan,nan])]
    for dtype in (3,4):
        maximum=0x7fefffffffffffff
        for l,axis,q,table in [
            (2,[maximum|(1<<63),maximum,maximum],0,[0,0]),
            (3,[0,1,0],0,[0,0,0]),
            (2,[0,rn(F(1)),rn(F(1))],1,[0,rn(F(1))]),
            (2,[rn(F(1)),0,rn(F(-1))],1,[rn(F(1)),0]),
            (2,[(1<<63)|1,1,2],0,[maximum,maximum|(1<<63)]),
        ]:
            yield 0,dtype,0,[1],l,1,[(0,)],[(3,[q]),(3,table),(3,axis)]
    # Rank-eight coordinates and finite Float32 cancellation between huge y.
    for dtype in (3,4):
        shape=[1,1,2,1,1,1,1,2]
        yield 1,dtype,0,shape,2,2,[(0,0,1,0,0,0,0,1)],[(3,enc([0,1,F(1,4),F(1,2)],3)),(3,enc([0,10,2,8],3)),(3,enc([0,1,1],3))]
        yield 0,dtype,0,[1],2,1,[(0,)],[(3,[rn(F(1,2))]),(3,[0x7fefffffffffffff,0xffefffffffffffff]),(3,enc([0,1,1],3))]


def main():
    rows=list(cases());text=[];expected=[]
    for channels,dtype,policy,shape,l,c,coords,ports in rows:
        line=' '.join(map(str,[channels,dtype,policy,len(shape),l,c,len(coords),*shape,*[x for at in coords for x in at]]))
        for typ,data in ports:line+=f' {typ} '+' '.join(f'{v:x}' for v in data)
        text.append(line);expected.append(reference(channels,dtype,policy,shape,l,c,coords,ports))
    profile=sys.argv[2] if len(sys.argv)>2 else 'strict'
    result=subprocess.run([sys.argv[1],profile,'oracle'],input='\n'.join(text)+'\n',text=True,capture_output=True,check=True)
    actual=result.stdout.splitlines();assert len(actual)==len(expected),(len(actual),len(expected),result.stderr[:1500])
    for i,(got,want) in enumerate(zip(actual,expected)):
        assert got==want,(i,rows[i],got,want,result.stderr[:1500])
    print(f'{len(rows)} independent Fraction grid/linear LUT1D cases passed ({profile})')


if __name__=='__main__':main()
