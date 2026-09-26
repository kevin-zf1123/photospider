#!/usr/bin/env python3
"""Independent real-formula oracle for the pinned FMT-09 contract.

Development-only dependency: mpmath. Runtime/build/CTest need no Python package.
Every committed expected bit pattern must agree at 180 AND 360 decimal digits.
Inputs and Float64 parameters are imported exactly; decimal constants are
constructed from integer ratios. Output rounding is direct integer RN-even,
not float64 -> float32. This numerical cross-check is not a formal CR proof.
"""
import argparse
import math
import random
import struct
from pathlib import Path
import mpmath as mp

NAMES = ['linear','power_gamma','srgb','bt709','bt2020','bt1886','pq','hlg_oetf','acescc','acescct']

def bits(x, narrow):
    try: return int.from_bytes(struct.pack('<f' if narrow else '<d', x),'little')
    except OverflowError: return 0x7f800000 if narrow else 0x7ff0000000000000

def value(b, narrow):
    return struct.unpack('<f' if narrow else '<d', int(b).to_bytes(4 if narrow else 8,'little'))[0]

def exact(x):
    n,d = x.as_integer_ratio()
    return mp.mpf(n)/d

def rn(x, narrow, negative_zero=False):
    f, bias, emax = (23,127,127) if narrow else (52,1023,1023)
    sign = (1 << (31 if narrow else 63)) if x < 0 or (x == 0 and negative_zero) else 0
    x = abs(x)
    if not x: return sign
    if mp.isinf(x): return sign | ((2*bias+1) << f)
    _, e = mp.frexp(x); e = int(e)-1
    if e > emax: return sign | ((2*bias+1) << f)
    shift = max(e,1-bias)-f
    scaled = mp.ldexp(x,-shift)
    n = int(mp.floor(scaled)); rem = scaled-n
    if rem > mp.mpf('0.5') or (rem == mp.mpf('0.5') and n&1): n += 1
    if n == (1 << (f+1)): n >>= 1; e += 1
    if e > emax: return sign | ((2*bias+1) << f)
    if e < 1-bias: return sign | n
    return sign | ((e+bias) << f) | (n-(1<<f))

def real(curve, encode, x, gamma, variant, lb, lw, beta):
    q=lambda n,d=1: mp.mpf(n)/d
    if curve == 0: return x
    if curve in (1,2,3,4):
        sign = -1 if x < 0 else 1; x = abs(x)
        if curve == 1: y = x ** (1/gamma if encode else gamma)
        elif curve == 2:
            if encode: y = q(323,25)*x if x <= q(7827,2500000) else q(211,200)*x**q(5,12)-q(11,200)
            else: y = q(25,323)*x if x <= q(809,20000) else ((x+q(11,200))/q(211,200))**q(12,5)
        else:
            if curve == 3 or variant == 1: a,b = q(1099,1000),q(18,1000)
            elif variant == 2: a,b = q(10993,10000),q(181,10000)
            else: b=beta; a=1+q(11,2)*b
            if encode: y = q(9,2)*x if x < b else a*x**q(9,20)-(a-1)
            else: y = q(2,9)*x if x < q(9,2)*b else ((x+a-1)/a)**q(20,9)
        return sign*y
    if curve == 5:
        black=lb**q(5,12); k=lw**q(5,12)-black
        return (x**q(5,12)-black)/k if encode else max(k*x+black,mp.mpf(0))**q(12,5)
    if curve == 6:
        m1,m2,c1,c2,c3=q(2610,16384),q(2523,32),q(3424,4096),q(2413,128),q(2392,128)
        if encode:
            y=(x/10000)**m1
            return ((c1+c2*y)/(1+c3*y))**m2
        z=x**(1/m2)
        return 10000*(max(z-c1,mp.mpf(0))/(c2-c3*z))**(1/m1)
    if curve == 7:
        a=q(17883277,100000000); b=1-4*a; c=q(1,2)-a*mp.log(4*a)
        if encode: return mp.sqrt(3*x) if x <= q(1,12) else a*mp.log(12*x-b)+c
        return x*x/3 if x <= q(1,2) else (mp.exp((x-c)/a)+b)/12
    A=q(105402377416545,10000000000000); B=q(729055341958355,10000000000000000)
    if encode:
        if curve == 8:
            if x <= 0: return q(-157,438)
            if x < q(1,32768): return (mp.log(q(1,65536)+x/2,2)+q(243,25))/q(438,25)
        elif x <= q(1,128): return A*x+B
        return (mp.log(x,2)+q(243,25))/q(438,25)
    h=(mp.log(65504,2)+q(243,25))/q(438,25)
    if x >= h: return mp.mpf(65504)
    if curve == 8:
        y=mp.power(2,q(438,25)*x-q(243,25))
        return 2*(y-q(1,65536)) if x <= q(-22,73) else y
    if x <= q(155251141552511,1000000000000000): return (x-B)/A
    return mp.power(2,q(438,25)*x-q(243,25))

def cases():
    rng=random.Random(9009)
    definitions=[(k,2.2,0,0.1,100.0) for k in range(10)]
    definitions += [(1,g,0,0.1,100.) for g in (1.,2.,0.5,1./2.4)]
    definitions += [(4,2.2,v,0.1,100.) for v in (1,2)]
    definitions += [(5,2.2,0,0.,1.),(5,2.2,0,0.,1000.)]
    for k,g,v,lb,lw in definitions:
        for encode in (False,True):
            for narrow in (False,True):
                xs=[0.,-0.,1.,0.1,0.18,0.5]
                if k in (0,1,2,3,4): xs += [-1.,-0.18,2.,4.,100.,1e-12,1e-30]
                if k == 5: xs += [lb,lw,lb+0.01,(lb+lw)/2] if encode else [0.01,0.75,0.999]
                if k == 6: xs += [1e-20,1e-8,1e-6,0.75,100.,1000.,10000.] if encode else [1e-20,1e-8,1e-6,0.75,0.999]
                if k in (8,9): xs += [-100.,-1.,-0.36,-0.301,0.0078125,0.155251141552511,1.467,2.,65504.]
                if k == 7 and not encode: xs += [-1.,-0.1,2.]
                xs += [rng.random()*(10000 if k == 6 and encode else 1) for _ in range(8)]
                cuts={2:[0.0031308 if encode else 0.04045],
                      3:[0.018 if encode else 0.081],
                      4:[(0.01805396851080781 if v == 0 else 0.018 if v == 1 else 0.0181)*(1 if encode else 4.5)],
                      7:[1/12 if encode else 0.5],
                      8:[0.,2**-15] if encode else [-22/73,1.4679963120447153],
                      9:[1/128] if encode else [0.155251141552511,1.4679963120447153]}.get(k,[])
                bs={bits(x,narrow) for x in xs}
                for cut in cuts:
                    b=bits(cut,narrow)
                    bs.update([b,max(0,b-1),b+1])
                # Dtype endpoints, including gradual underflow and huge values.
                if k != 5:
                    bs.update([1,0x00800000 if narrow else 0x0010000000000000])
                if k in (0,1,2,3,4,8,9): bs.add(0x7f7fffff if narrow else 0x7fefffffffffffff)
                for b in sorted(bs):
                    x=value(b,narrow)
                    if not math.isfinite(x): continue
                    yield (k,int(encode),int(narrow),g,v,lb,lw,b)

def generate():
    cs=list(cases()); results=[]
    for precision in (180,360):
        mp.mp.dps=precision
        beta=mp.findroot(lambda b: 10*b**(mp.mpf(11)/20)-1-mp.mpf(11)*b/2,(mp.mpf('.018'),mp.mpf('.019')))
        current=[]
        for k,e,n,g,v,lb,lw,b in cs:
            x=value(b,n)
            y=real(k,e,exact(x),exact(g),v,exact(lb),exact(lw),beta)
            if isinstance(y,mp.mpc): raise ValueError((k,e,n,x,y))
            negative_zero=(b >> (31 if n else 63)) != 0 and k in (0,1,2,3,4)
            current.append(b if k == 0 or (k == 1 and g == 1) else rn(y,n,negative_zero))
        if results: assert results == current, 'oracle precision disagreement'
        results=current
    out=['// Generated by tests/oracles/fmt09_reference.py; 180/360 dps agreement.',
         '#pragma once','#include <cstdint>','namespace fmt09_test {',
         'struct Case { unsigned curve; bool encode, narrow; double gamma; unsigned variant; double black, white; std::uint64_t input, expected; };',
         'inline constexpr Case golden[] = {']
    for c,r in zip(cs,results):
        k,e,n,g,v,lb,lw,b=c
        out.append(f'  {{{k}, {str(bool(e)).lower()}, {str(bool(n)).lower()}, {g.hex()}, {v}, {lb.hex()}, {lw.hex()}, UINT64_C(0x{b:016x}), UINT64_C(0x{r:016x})}},')
    return '\n'.join(out+['};','}  // namespace fmt09_test',''])

if __name__ == '__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--check',action='store_true'); args=parser.parse_args()
    path=Path(__file__).resolve().parents[1]/'fixtures/fmt09_golden.hpp'
    text=generate()
    if args.check:
        assert path.read_text()==text, 'stale golden fixture'
        print('FMT-09 independent golden fixture verified')
    else:
        path.write_text(text); print(f'Wrote {text.count("UINT64_C")//2} golden cases to {path}')
