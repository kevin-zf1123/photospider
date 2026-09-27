"""Directed MPFR complete-kernel oracle for the manual uniform workflow.

Exact logical support/IEEE ordering uses Fraction; finite coefficients and the
entire normalized sum use MPFR enclosures. No Float64-rounded coefficient is
used as the reference. Kaiser uses a positive I0 series with a geometric tail.
"""
import math
import random
import subprocess
import sys
from fractions import Fraction as F
from comparison_oracle import number
from math_oracle_support import MPFR, multiply_bounds, negate_bounds
from curve_oracle import bits


def tap_sign(kernel, radius, parameter, j):
    if not j or kernel == 4:
        return 1
    if kernel in (0, 2) and abs(j) == radius:
        return 0
    z = 2*parameter*abs(j)
    if z.denominator == 1:
        return 0
    return -1 if (z.numerator//z.denominator) % 2 else 1


def mapped(index, count, boundary):
    if 0 <= index < count:
        return index
    if boundary == 2:
        return None
    if boundary == 1:
        return max(0, min(count-1,index))
    if boundary == 3:
        return index % count
    if count == 1:
        return 0
    x = index % (2*(count-1))
    return x if x < count else 2*(count-1)-x


class Bounds:
    def __init__(self, oracle):
        self.o = oracle
    def rational(self, value):
        value = F(value)
        a,b = self.o.integer(value.numerator),self.o.integer(value.denominator)
        return (self.o.binary('div',a,b,self.o.downward),self.o.binary('div',a,b,self.o.upward))
    def add(self,a,b):
        return (self.o.binary('add',a[0],b[0],self.o.downward),self.o.binary('add',a[1],b[1],self.o.upward))
    def multiply(self,a,b):
        return multiply_bounds(self.o,a,b)
    def divide(self,a,b):
        return multiply_bounds(self.o,a,b,divide=True)
    def negative(self,a):
        return negate_bounds(self.o,a)
    def bessel(self,z):
        term = total = self.rational(1)
        threshold = self.rational(F(2)**(-self.o.precision))
        half = self.rational(F(1,2))
        for n in range(1,4096):
            term = self.divide(self.multiply(term,z),self.rational(n*n))
            ratio = self.divide(z,self.rational((n+1)**2))
            if self.o.compare(ratio[1],half[0]) <= 0 and self.o.compare(term[1],threshold[0]) < 0:
                tail = self.divide(term,self.add(self.rational(1),self.negative(ratio)))
                return total[0],self.o.binary('add',total[1],tail[1],self.o.upward)
            total = self.add(total,term)
        raise ArithmeticError('I0 reference tail')
    def coefficient(self,kernel,radius,parameter,beta,j):
        if kernel == 4:
            exponent = self.rational(-F(j*j)/(2*parameter*parameter))
            return self.o.unary('exp',exponent[0],self.o.downward),self.o.unary('exp',exponent[1],self.o.upward)
        if not j:
            sinc = self.rational(1)
        else:
            z = self.rational(2*parameter*j)
            # This phase is an exact dyadic, fitting the selected precision.
            assert self.o.compare(z[0],z[1]) == 0
            sine = self.o.unary('sinpi',z[0],self.o.downward),self.o.unary('sinpi',z[1],self.o.upward)
            pi = self.o.pi(self.o.downward),self.o.pi(self.o.upward)
            sinc = self.divide(sine,self.multiply(pi,z))
        if kernel == 3:
            window = self.bessel(self.rational(beta*beta*(1-F(j*j,radius*radius))/4))
        else:
            u = self.rational(F(j,radius))
            cosine = self.o.unary('cospi',u[1],self.o.downward),self.o.unary('cospi',u[0],self.o.upward)
            if kernel == 0:
                window = self.divide(self.add(self.rational(1),cosine),self.rational(2))
            elif kernel == 1:
                window = self.add(self.rational(F(27,50)),self.multiply(self.rational(F(23,50)),cosine))
            else:
                # Independent double-angle expression, not the C++ factorization.
                cos2 = self.add(self.multiply(self.rational(2),self.multiply(cosine,cosine)),self.rational(-1))
                window = self.add(self.add(self.rational(F(21,50)),self.multiply(self.rational(F(1,2)),cosine)),self.multiply(self.rational(F(2,25)),cos2))
        return self.multiply(sinc,window)


def reference(kernel,dtype,radius,boundary,parameter_raw,beta_raw,raw):
    parameter,beta = number(parameter_raw,3),number(beta_raw,3)
    signbit = 1 << (31 if dtype == 4 else 63)
    inf = 0x7f800000 if dtype == 4 else 0x7ff0000000000000
    quiet = 1 << (22 if dtype == 4 else 51)
    selected = []
    for center in range(len(raw)):
        selected.append([(j,0 if mapped(center+j,len(raw),boundary) is None else raw[mapped(center+j,len(raw),boundary)],tap_sign(kernel,radius,parameter,j)) for j in range(-radius,radius+1) if tap_sign(kernel,radius,parameter,j)])
    results = [None]*len(raw)
    for i,taps in enumerate(selected):
        for _,sample,_ in taps:
            if number(sample,dtype) is None:
                results[i] = sample|quiet
                break
        if results[i] is not None:
            continue
        signs = set((number(sample,dtype)>0) == (sign>0) for _,sample,sign in taps if number(sample,dtype) in (math.inf,-math.inf))
        if signs:
            results[i] = inf|quiet if len(signs)==2 else inf|(0 if True in signs else signbit)
        elif all(sample==taps[0][1] for _,sample,_ in taps):
            results[i] = taps[0][1]
        else:
            by_j = {j:number(sample,dtype) for j,sample,_ in taps}
            if all(by_j[-j]+by_j[j]==2*by_j[0] for j in by_j if j>0):
                results[i] = bits(by_j[0],dtype)
    for precision in (128,256,512,1024,2048,4096,8192):
        if all(v is not None for v in results):
            break
        with MPFR(precision) as oracle:
            b = Bounds(oracle)
            coefficients = {j:b.coefficient(kernel,radius,parameter,beta,j) for j in range(radius+1) if tap_sign(kernel,radius,parameter,j)}
            denominator = b.rational(0)
            for j,coefficient in coefficients.items():
                denominator = b.add(denominator,b.multiply(b.rational(2 if j else 1),coefficient))
            if oracle.compare(denominator[0],oracle.integer(0)) <= 0:
                continue
            for i,taps in enumerate(selected):
                if results[i] is not None:
                    continue
                total = b.rational(0)
                for j,sample,_ in taps:
                    source = oracle.raw(sample,dtype)
                    total = b.add(total,b.multiply(coefficients[abs(j)],(source,source)))
                quotient = b.divide(total,denominator)
                a,z = (oracle.bits(value,dtype) for value in quotient)
                if a == z:
                    results[i] = a
    assert all(v is not None for v in results),(kernel,dtype,radius,boundary,parameter,beta,raw,results)
    return ' '.join(f'{v:x}' for v in results)


def cases():
    rng = random.Random(0xC11)
    for kernel in range(5):
        for dtype in (3,4):
            for radius in (1,2,5):
                for boundary in range(4):
                    for pattern in range(2):
                        values = [F(rng.randrange(-20,21),8) for _ in range(7)] if pattern else [F(0),F(0),F(0),F(1),F(0),F(0),F(0)]
                        yield kernel,dtype,radius,boundary,bits(F(3,2) if kernel==4 else F(1,4)),bits(4),[bits(v,dtype) for v in values]
            sign = 1 << (31 if dtype==4 else 63)
            inf = 0x7f800000 if dtype==4 else 0x7ff0000000000000
            for boundary in range(4):
                for raw in ([sign]*3,[inf,0,inf|sign],[inf|42,inf|sign|56,0],[1,3,5,3,1]):
                    yield kernel,dtype,2,boundary,bits(1 if kernel==4 else F(3,16)),bits(0),raw
            # Exact quarter-wave and Nyquist sinusoids measure the full
            # periodic discrete response without input trigonometric rounding.
            for wave in ((0,1,0,-1)*2,(1,-1)*4):
                yield kernel,dtype,2,3,bits(1 if kernel==4 else F(1,4)),bits(3),[bits(v,dtype) for v in wave]
            # Repeated reflected logical taps can give one infinite source
            # both coefficient signs; source order cannot be deduplicated.
            for values in ([inf,0],[inf|42,inf|sign|56],[sign]):
                yield kernel,dtype,7,0,bits(1 if kernel==4 else F(2,5)),bits(0),values
            if kernel==4:
                for sigma in (F(2)**-1000,F(2)**500):
                    yield kernel,dtype,2,0,bits(sigma),bits(0),[bits(v,dtype) for v in (1,3,2)]
            for param in ((F(1,100),F(20)) if kernel==4 else (F(1,1024),F(511,1024))):
                yield kernel,dtype,3,0,bits(param),bits(20),[bits(v,dtype) for v in (1,-3,5,-7,5,-3,1)]


from accuracy_oracle import accepted_values

def main():
    rows = list(cases())
    expected,lines = [],[]
    for row in rows:
        k,t,r,b,p,beta,values = row
        expected.append(reference(*row))
        lines.append(f'{k} {t} {r} {b} {len(values)} {p:x} {beta:x} '+' '.join(f'{v:x}' for v in values))
    profile = sys.argv[2] if len(sys.argv)>2 else 'strict'
    result = subprocess.run([sys.argv[1],profile,'oracle'],input='\n'.join(lines)+'\n',text=True,capture_output=True,check=True)
    actual = result.stdout.splitlines()
    assert len(actual)==len(expected),(len(actual),len(expected),result.stderr)
    for i,(got,want) in enumerate(zip(actual,expected)):
        assert accepted_values(got,want,rows[i][1],profile),(i,rows[i],got,want,result.stderr[:1000])
    print(f'{len(rows)} independent directed MPFR uniform lowpass cases passed ({profile})')


if __name__ == '__main__':
    main()
