"""Independent exact Bernstein oracle after separately rounded controls."""
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
    answer = ieee_round(v, 32 if dtype == 4 else 64, negative)
    if answer is None:
        raise OverflowError
    return answer


def reference(degree, k, n, d, dtype, requested, ports):
    observed = requested
    requested = [(i, c) for i in range(n) for c in range(d)] if requested else []
    source = [[number(b, t) for b in data] for t, data in ports]
    negative = lambda p, i: bool(ports[p][1][i] >> (31 if ports[p][0] == 4 else 63))
    try:
        # All Whole row controls are classified before component arithmetic.
        for i in sorted({i for i, c in requested}):
            j, t = source[2][i], source[3][i]
            if j < 0 or j >= k - 1 or not finite(t) or not 0 <= t <= 1:
                return 'query'
        out = {}
        for i, c in sorted(requested):
            j, t = source[2][i], source[3][i]
            if t in (0, 1):
                pos = (j + (t == 1)) * d + c
                v = source[0][pos]
                if not finite(v):
                    return 'domain'
                out[i, c] = rn(v, dtype, negative(0, pos))
                continue
            a, b = source[0][j*d+c], source[0][(j+1)*d+c]
            if not finite(a) or not finite(b):
                return 'domain'
            controls, signs = [a], [negative(0, j*d+c)]
            for h in range(degree-1):
                pos = (j*(degree-1)+h)*d+c
                offset = source[1][pos]
                if not finite(offset):
                    return 'domain'
                anchor = b if h else a
                nz = not anchor and not offset and negative(0, (j+(h>0))*d+c) and negative(1, pos)
                bits = rn(anchor+offset, 3, nz)
                controls.append(number(bits, 3)); signs.append(bool(bits >> 63))
            controls.append(b); signs.append(negative(0, (j+1)*d+c))
            exact = sum(F(math.comb(degree, r))*(1-t)**(degree-r)*t**r*v for r, v in enumerate(controls))
            out[i, c] = rn(exact, dtype, not exact and all(not v and neg for v, neg in zip(controls, signs)))
        return ''.join(f'{out[at]:x} ' for at in observed)
    except OverflowError:
        return 'overflow'


def cases():
    rng = random.Random(20303)
    enc = lambda values, t: [rn(F(v), t) for v in values]
    for degree in (2, 3):
        for dtype in (3, 4):
            for trial in range(180):
                k, n, d = rng.randrange(2, 6), rng.randrange(1, 7), rng.randrange(1, 6)
                types = [rng.choice((3, 4)), rng.choice((3, 4)), 2, rng.choice((3, 4))]
                a = [F(rng.randrange(-100, 101), 8) for _ in range(k*d)]
                h = [F(rng.randrange(-100, 101), 8) for _ in range((k-1)*(degree-1)*d)]
                js = [rng.randrange(k-1) for _ in range(n)]
                ts = [rng.choice((F(0), F(1), F(1, 2), F(1, 8), F(7, 8), F(1, 3))) for _ in range(n)]
                wanted = [(i, c) for i in range(n) for c in range(d) if trial%3 or rng.randrange(2)]
                ports = [(types[0], enc(a, types[0])), (types[1], enc(h, types[1])), (2, js), (types[3], enc(ts, types[3]))]
                yield degree, k, n, d, dtype, wanted, ports
    for degree in (2, 3):
        for dtype in (3, 4):
            for trial in range(120):
                # Broad exponent/range, t including subnormal and near endpoints.
                a = [rng.getrandbits(64) for _ in range(2)]
                h = [rng.getrandbits(64) for _ in range(degree-1)]
                a = [b if finite(number(b, 3)) else 0 for b in a]
                h = [b if finite(number(b, 3)) else 0 for b in h]
                t = rng.choice((1, 0x0010000000000000, 0x3fd5555555555555, 0x3fefffffffffffff, 0x3fe0000000000000))
                yield degree, 2, 1, 1, dtype, [(0, 0)], [(3, a), (3, h), (2, [0]), (3, [t])]
    nan = 0x7ff0000000000042
    for dtype in (3, 4):
        for port in range(4):
            for pos in range((6, 4, 3, 3)[port]):
                for i in range(3):
                    for c in range(2):
                        values = [enc([0, 1, 2, 3, 4, 5], 3), enc([1, 1, 1, 1], 3), [0, 0, 1], enc([0, F(1, 2), 1], 3)]
                        values[port][pos] = (1 << 64)-1 if port == 2 else nan
                        yield 2, 3, 3, 2, dtype, [(i, c)], [(2 if p == 2 else 3, v) for p, v in enumerate(values)]
        sign = 1 << 63
        for a in ([0, 0], [sign, sign], [0, sign]):
            for h in (0, sign, 1, sign|1):
                yield 2, 2, 3, 1, dtype, [(0, 0), (1, 0), (2, 0)], [(3, a), (3, [h]), (2, [0, 0, 0]), (3, [sign, rn(F(1,2)), rn(F(1))])]
        for a,h in [([rn(F(1)),rn(F(1))+2],[rn(F(1,1<<53))]),
                     ([0x7fefffffffffffff,0xffefffffffffffff],[0xffefffffffffffff])]:
            yield 2,2,1,1,dtype,[(0,0)],[(3,a),(3,h),(2,[0]),(3,[rn(F(1,2))])]
        for t in [nan, 0x7ff0000000000000, rn(F(-1)), rn(F(2))]:
            yield 2,2,1,1,dtype,[(0,0)],[(3,[0,0]),(3,[nan]),(2,[0]),(3,[t])]


def main():
    rows = list(cases()); text = []; expected = []
    for degree,k,n,d,dtype,wanted,ports in rows:
        header = [degree,k,n,d,dtype,len(wanted)] + [x for at in wanted for x in at]
        line = ' '.join(map(str, header))
        for typ,data in ports:
            line += f' {typ} ' + ' '.join(f'{b:x}' for b in data)
        text.append(line); expected.append(reference(degree,k,n,d,dtype,wanted,ports))
    profile = sys.argv[2] if len(sys.argv)>2 else 'strict'
    result = subprocess.run([sys.argv[1],profile,'oracle'],input='\n'.join(text)+'\n',text=True,capture_output=True,check=True)
    actual = result.stdout.splitlines()
    assert len(actual)==len(expected),(len(actual),len(expected),result.stderr)
    for i,(got,want) in enumerate(zip(actual,expected)):
        assert got==want,(i,rows[i],got,want,result.stderr[:1000])
    print(f'{len(rows)} independent exact Bernstein/RN64 parametric cases passed ({profile})')


if __name__ == '__main__':
    main()
