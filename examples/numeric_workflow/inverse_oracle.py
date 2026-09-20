"""Independent Fraction Hermite inversion, including exact IEEE midpoint roots.

Run: python3 inverse_oracle.py /path/to/photospider_numeric_inverse strict
The reference solves in normalized t, without rounded slopes or forward values.
"""
import bisect
import math
import random
import subprocess
import sys
from fractions import Fraction as F
from comparison_oracle import number
from curve_oracle import slopes
from sequence_oracle import ieee_round


def bits(value, dtype=3, negative_zero=False):
    result = ieee_round(F(value), 32 if dtype == 4 else 64, negative_zero)
    if result is not None:
        return result
    return (0x7f800000 if dtype == 4 else 0x7ff0000000000000) | ((1 << (31 if dtype == 4 else 63)) if value < 0 else 0)


def inverse_root(x0, x1, y0, y1, m0, m1, q, out):
    h = x1-x0
    direction = 1 if y1 > y0 else -1
    def polynomial(v):
        t = (v-x0)/h
        return ((2*t**3-3*t**2+1)*y0 + (t**3-2*t**2+t)*h*m0
                + (-2*t**3+3*t**2)*y1 + (t**3-t**2)*h*m1-q)*direction
    # Independent rational bisection in real x. Once rounded endpoints are
    # adjacent, their midpoint is the only remaining rounding decision.
    low, high = x0, x1
    signbit = 1 << (31 if out == 4 else 63)
    def key(v):
        return signbit-(v & (signbit-1)) if v & signbit else signbit+v
    for _ in range(5000):
        a, b = bits(low, out), bits(high, out)
        if a == b:
            return a
        if key(b)-key(a) <= 1:
            va, vb = number(a, out), number(b, out)
            if va == -math.inf:
                midpoint = vb-F(2)**(103 if out == 4 else 970)
            elif vb == math.inf:
                midpoint = va+F(2)**(103 if out == 4 else 970)
            else:
                midpoint = (va+vb)/2
            value = polynomial(midpoint)
            chosen = a if value > 0 else b if value < 0 else a if not a & 1 else b
            if chosen & (signbit-1) == 0:
                # A root exactly at zero is canonical +0. Other roots retain
                # their actual sign even when both rounded bounds are zero.
                at_zero = polynomial(F(0))
                chosen = signbit if at_zero > 0 else 0
            return chosen
        middle = (low+high)/2
        value = polynomial(middle)
        if not value:
            return bits(middle, out)
        if value < 0:
            low = middle
        else:
            high = middle
    raise AssertionError('reference did not isolate root')


def reference(pchip, out, policy, xt, yt, qt, xr, yr, qr):
    x = [number(v, xt) for v in xr]
    y = [number(v, yt) for v in yr]
    query = [number(v, qt) for v in qr]
    finite = lambda v: v is not None and v not in (math.inf, -math.inf)
    if not all(map(finite, x+y)) or any(a >= b for a,b in zip(x,x[1:])):
        return 'domain'
    increasing = y[-1] > y[0]
    if any(a >= b if increasing else a <= b for a,b in zip(y,y[1:])):
        return 'domain'
    if not all(map(finite, query)) or (not policy and any(q < min(y) or q > max(y) for q in query)):
        return 'domain'
    ordered = y if increasing else [-v for v in y]
    derivatives = slopes(x, y)
    answer = []
    for q in query:
        index = bisect.bisect_left(ordered, q if increasing else -q)
        selected = index if index < len(y) and y[index] == q else None
        if selected is None and (index == 0 or index == len(y)):
            selected = 0 if not index else len(y)-1
        if selected is not None:
            raw = bits(x[selected], out, not x[selected] and bool(xr[selected] >> (31 if xt == 4 else 63)))
        else:
            j = index-1
            if not pchip or len(x) == 2:
                exact = x[j]+(q-y[j])*(x[j+1]-x[j])/(y[j+1]-y[j])
                raw = bits(exact, out)
            else:
                raw = inverse_root(x[j], x[j+1], y[j], y[j+1], derivatives[j], derivatives[j+1], q, out)
        if number(raw, out) in (math.inf, -math.inf):
            return 'overflow'
        answer.append(f'{raw:x}')
    return ' '.join(answer)


def cases():
    rng = random.Random(0xC10)
    for pchip in (0, 1):
        for out in (3, 4):
            for direction in (-1, 1):
                for xt in (3, 4):
                    for yt in (3, 4):
                        for qt in (3, 4):
                            for k in (2, 3, 5, 7):
                                x = [F(0)]
                                y = [F(-3)]
                                for _ in range(k-1):
                                    x.append(x[-1]+F(rng.randrange(1,20), 8))
                                    y.append(y[-1]+F(rng.randrange(1,20), 8))
                                y = [direction*v for v in y]
                                q = [y[0], y[-1], y[1]]+[y[0]+(y[-1]-y[0])*F(i,32) for i in (1,2,15,16,17,30,31)]
                                rng.shuffle(q)
                                yield pchip,out,0,xt,yt,qt,[bits(v,xt) for v in x],[bits(v,yt) for v in y],[bits(v,qt) for v in q]
                for policy in (0,1):
                    yield pchip,out,policy,3,3,3,[1<<63,bits(1),bits(2)],[bits(direction*v) for v in (0,1,4)],[bits(direction*v) for v in (-1,0,1,4,5)]
                # Affine PCHIP yields exact normal ties and signed subnormal ties.
                for scale in (F(1), F(2)**-1074, F(2)**-149, F(2)**129):
                    x = [-scale, F(0), scale]
                    y = [-F(2), F(0), F(2)]
                    q = [-F(1), F(0), F(1), F(2)**-52, 1-F(2)**-25]
                    yield pchip,out,0,3,3,3,[bits(v) for v in x],[bits(direction*v) for v in y],[bits(direction*v) for v in q]
                yield pchip,out,0,3,3,3,[bits(0),bits(1),bits(2)],[bits(direction*v) for v in (-2,0,2)],[bits(direction*F(i)*F(2)**-52) for i in (1,3)]
                # Exactly half-minsubnormal with Float64-scale knots and values.
                yield pchip,out,0,3,3,3,[(1<<63)|1,0,1],[bits(direction*v*F(2)**-1074) for v in (-2,0,2)],[bits(direction*v*F(2)**-1074) for v in (-1,1)]
                # Positive derivative approaches zero at x=0; residual alone
                # cannot bound inverse error. Include both sides of query knots.
                for exponent in (-1000,-500,-100,0,100,500,1000):
                    scale = F(2)**exponent
                    x = [0,scale,2*scale,3*scale]
                    y = [F(0),F(1),F(4),F(9)]
                    q = [F(2)**-1074,F(2)**-100, F(1,16), F(15,16), F(17,16), F(5)]
                    yield pchip,out,0,3,3,3,[bits(v) for v in x],[bits(direction*v) for v in y],[bits(direction*v) for v in q]
            # Narrow consecutive knot intervals and huge slope variation.
            for base in (0x3ff0000000000000, 0x0010000000000000, 0x7fefffffffffff00):
                yield pchip,out,0,3,3,3,[base+i for i in (0,1,2,8)],[bits(v) for v in (0,1,4,9)],[bits(F(v,16)) for v in (1,3,15,17,63,65,143)]
            for bad in ([0,1,1], [0,2,1], [0,1,math.inf], [0,1,math.nan]):
                rawbad = [0x7ff0000000000000 if v == math.inf else 0x7ff0000000000042 if isinstance(v,float) and math.isnan(v) else bits(v) for v in bad]
                yield pchip,out,0,3,3,3,[0,bits(1),bits(2)],rawbad,[0]


def main():
    rows = list(cases())
    encoded, expected = [], []
    for pchip,out,policy,xt,yt,qt,x,y,q in rows:
        line = f'{pchip} 0 {out} {policy} {len(x)} {len(q)} 1'
        for dtype, values in ((xt,x),(yt,y),(qt,q)):
            line += f' {dtype} '+' '.join(f'{v:x}' for v in values)
        encoded.append(line)
        expected.append(reference(pchip,out,policy,xt,yt,qt,x,y,q))
    profile = sys.argv[2] if len(sys.argv) > 2 else 'strict'
    result = subprocess.run([sys.argv[1],profile,'oracle'], input='\n'.join(encoded)+'\n', text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual),len(expected),result.stderr)
    for i,(got,want) in enumerate(zip(actual,expected)):
        assert got == want, (i,rows[i],got,want,result.stderr[:1000])
    print(f'{len(rows)} independent Fraction inverse cases passed ({profile})')


if __name__ == '__main__':
    main()
