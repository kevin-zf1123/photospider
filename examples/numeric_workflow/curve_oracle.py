"""Independent exact Fraction PCHIP slopes/Hermite and linear curve oracle."""
import bisect
import math
import random
import subprocess
import sys
from fractions import Fraction
from comparison_oracle import number
from sequence_oracle import ieee_round


def bits(value, dtype=3, negative_zero=False):
    return ieee_round(Fraction(value), 32 if dtype == 4 else 64, negative_zero)


def slopes(x, y):
    h = [b-a for a,b in zip(x,x[1:])]
    d = [(b-a)/width for a,b,width in zip(y,y[1:],h)]
    if len(x) == 2:
        return [d[0],d[0]]
    def sign(v):
        return (v > 0)-(v < 0)
    def endpoint(h0,h1,d0,d1):
        e = ((2*h0+h1)*d0-h0*d1)/(h0+h1)
        if sign(e) != sign(d0):
            return Fraction()
        if sign(d0) != sign(d1) and abs(e) > 3*abs(d0):
            return 3*d0
        return e
    result = [endpoint(h[0],h[1],d[0],d[1])]
    for i in range(1,len(x)-1):
        if not d[i-1] or sign(d[i-1]) != sign(d[i]):
            result.append(Fraction())
        else:
            w1,w2 = 2*h[i]+h[i-1],h[i]+2*h[i-1]
            result.append((w1+w2)/(w1/d[i-1]+w2/d[i]))
    result.append(endpoint(h[-1],h[-2],d[-1],d[-2]))
    return result


def reference(pchip, multi, output, policy, xt, yt, qt, xr, yr, qr, columns):
    x = [number(v,xt) for v in xr]
    y = [number(v,yt) for v in yr]
    query = [number(v,qt) for v in qr]
    finite = lambda v: v is not None and v not in (math.inf,-math.inf)
    if not all(map(finite,x)) or any(a >= b for a,b in zip(x,x[1:])):
        return 'domain'
    # A regional execution validates all requested controls before any y Need.
    if any(not finite(q) or (not policy and (q < x[0] or q > x[-1])) for q in query):
        return 'domain'
    answer = []
    for q in query:
        if not finite(q):
            return 'domain'
        insertion = bisect.bisect_left(x,q)
        selected = insertion if insertion < len(x) and x[insertion] == q else None
        exterior = q < x[0] or q > x[-1]
        if exterior and not policy:
            return 'domain'
        if exterior and policy == 1:
            selected = 0 if q < x[0] else len(x)-1
        segment = max(0,min(len(x)-2,insertion-1))
        for c in range(columns):
            if selected is not None:
                exact = y[selected*columns+c]
                if not finite(exact):
                    return 'domain'
                negative_zero = not exact and bool(yr[selected*columns+c] >> (31 if yt == 4 else 63))
            else:
                first = max(0,segment-1) if pchip else segment
                end = min(len(x),segment+3) if pchip else segment+2
                local = [y[j*columns+c] for j in range(first,end)]
                if not all(map(finite,local)):
                    return 'domain'
                y0,y1 = y[segment*columns+c],y[(segment+1)*columns+c]
                if not pchip or len(x) == 2:
                    t = (q-x[segment])/(x[segment+1]-x[segment])
                    exact = (1-t)*y0+t*y1
                else:
                    # Only local y is valid. Extend unused entries with zero;
                    # local slope formulas for the selected segment are unchanged.
                    column = [y[j*columns+c] if first <= j < end else Fraction() for j in range(len(x))]
                    m = slopes(x,column)
                    if exterior:
                        j = 0 if q < x[0] else len(x)-1
                        exact = column[j]+(q-x[j])*m[j]
                    else:
                        h = x[segment+1]-x[segment]
                        t = (q-x[segment])/h
                        exact = (2*t**3-3*t*t+1)*y0+(t**3-2*t*t+t)*h*m[segment]+(-2*t**3+3*t*t)*y1+(t**3-t*t)*h*m[segment+1]
                sign = 1 << (31 if yt == 4 else 63)
                negative_zero = not exact and yr[segment*columns+c] == sign and yr[(segment+1)*columns+c] == sign
            value = bits(exact,output,negative_zero)
            if value is None:
                return 'overflow'
            answer.append(f'{value:x}')
    return ' '.join(answer)


def cases():
    rng = random.Random(10101)
    for pchip in (0,1):
        for multi in (0,1):
            for output in (3,4):
                for policy in (0,1,2):
                    for trial in range(100):
                        k = rng.randrange(2,8)
                        c = rng.randrange(1,4) if multi else 1
                        xt,yt,qt = [rng.choice((3,4)) for _ in range(3)]
                        xx = sorted(rng.sample(range(-20,21),k))
                        xr = [bits(Fraction(v,4),xt) for v in xx]
                        sign = 1 << (31 if yt == 4 else 63)
                        infinity = 0x7f800000 if yt == 4 else 0x7ff0000000000000
                        pool = [0,sign,1,sign|1,infinity-1,sign|(infinity-1),bits(1,yt),bits(-1,yt)]
                        if trial % 9 == 0:
                            pool += [infinity,infinity|0x42]
                        yr = [rng.choice(pool) if trial % 3 == 0 else bits(Fraction(rng.randrange(-100,101),16),yt) for _ in range(k*c)]
                        q = [Fraction(rng.randrange(xx[0]*4-8,xx[-1]*4+9),16),Fraction(rng.choice(xx),4),Fraction(xx[0]+xx[-1],8)]
                        rng.shuffle(q)
                        qr = [bits(v,qt) for v in q]
                        yield pchip,multi,output,policy,xt,yt,qt,xr,yr,qr,c
        for output in (3,4):
            max64 = 0x7fefffffffffffff
            minsub = 1
            half = bits(Fraction(1,2))
            for xr,yr,qr in [
                ([max64|(1<<63),max64],[max64|(1<<63),max64],[0]),
                ([0,bits(1)],[max64|(1<<63),max64],[half]),
                ([0,1,2],[0,bits(1),bits(4)],[1]),
                ([0,2,4],[0,bits(1),bits(4)],[1,3]),
                ([bits(-1),0,bits(1),bits(2)],
                 [bits(-3*(1<<140)),bits(-(1<<140)),bits(1<<140),bits(3*(1<<140))],[half]),
                ([0,bits(1<<1022),bits(1<<1023)],[0,1,2],[bits(1<<1021)]),
                ([0,bits(1),bits(2)],[0,minsub,2],[half,bits(Fraction(3,2))]),
                ([0,bits(1)],[bits(1),bits(1)+(1 if output==3 else 1<<29)],[half,half-1,half+1]),
                ([0,bits(1)],[1,2],[half,half-1,half+1]),
                ([0,bits(1)],[1|(1<<63),2|(1<<63)],[half]),
                ([0,bits(1),bits(2)],[1<<63,1<<63,1<<63],[bits(-1),half,bits(3)]),
                ([0,bits(1),bits(2)],[1<<63,0,1<<63],[bits(-1),half,bits(3)]),
                ([0,1,2,bits(1)],[max64,0,max64,0],[bits(Fraction(1,2))]),
            ]:
                yield pchip,0,output,2,3,3,3,xr,yr,qr,1
            for k in (2,3,4,5):
                for direction in (-1,1):
                    xr = [bits(i) for i in range(k)]
                    yr = [bits(direction*i*i) for i in range(k)]
                    qr = sorted({bits(Fraction(i,16)) for i in range(16*(k-1)+1)} | {bits(i)+d for i in range(1,k-1) for d in (-1,0,1)})
                    yield pchip,0,output,0,3,3,3,xr,yr,qr,1


from accuracy_oracle import accepted_values

def main():
    q = bits(Fraction(float.fromhex('0x1.ff84b83a89299p-1')))
    regression = [(1,0,3,0,3,3,3,[bits(0),bits(1),bits(2)],
                   [bits(0),bits(1),bits(0)], query,1)
                  for query in ([q,q+1],[q],[q+1])]
    rows = regression + list(cases())
    encoded,wanted = [],[]
    for pchip,multi,out,policy,xt,yt,qt,x,y,q,c in rows:
        line = f'{pchip} {multi} {out} {policy} {len(x)} {len(q)} {c}'
        for dtype,raw in ((xt,x),(yt,y),(qt,q)):
            line += f' {dtype} '+' '.join(f'{v:x}' for v in raw)
        encoded.append(line)
        wanted.append(reference(pchip,multi,out,policy,xt,yt,qt,x,y,q,c))
    selected = sys.argv[2] if len(sys.argv)>2 else 'strict'
    result = subprocess.run([sys.argv[1],selected,'oracle'],input='\n'.join(encoded)+'\n',text=True,capture_output=True,check=True)
    actual = result.stdout.splitlines()
    assert len(actual)==len(wanted),(len(actual),len(wanted),result.stderr)
    for i,(got,want) in enumerate(zip(actual,wanted)):
        if i < len(regression):
            assert got == want, ('PCHIP monotonic mixed-path regression', got, want)
        assert accepted_values(got,want,rows[i][2],selected),(i,rows[i],got,want,result.stderr[:1000])
    print(f'{len(rows)} independent Fraction linear/PCHIP cases passed ({selected})')


if __name__ == '__main__':
    main()
