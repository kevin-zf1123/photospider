"""Independent exact reconstruction and certified continuous-kernel moments.

Boundary copies are enumerated with Fraction (not the runtime fold walker).
Hann/Hamming/Blackman use product-to-sum sinc identities; Gaussian and Kaiser
window polynomials have exact rational coefficients. MPFR enclosures retain pi
and whole-quotient error. All omitted terms have an explicit geometric bound.
"""
import ctypes as C
import math
import subprocess
import sys
from fractions import Fraction as F
from comparison_oracle import number
from curve_oracle import bits
from lowpass_oracle import Bounds
from math_oracle_support import MPFR, multiply_bounds
from sequence_oracle import ieee_round


class ExactBounds(Bounds):
    def rational(self,value):
        value = F(value)
        def integer(n,mode):
            result = self.o.value()
            self.o.lib.mpfr_set_str(C.byref(result),str(n).encode(),10,mode)
            return result
        n = integer(value.numerator,self.o.downward),integer(value.numerator,self.o.upward)
        d = integer(value.denominator,self.o.downward),integer(value.denominator,self.o.upward)
        return multiply_bounds(self.o,n,d,divide=True)
    def absolute(self,pair):
        a = self.o.unary('neg',pair[0],self.o.upward)
        hi = a if self.o.compare(a,pair[1]) > 0 else pair[1]
        return self.o.integer(0),hi
    def widen(self,pair,error):
        return (self.o.binary('sub',pair[0],error[1],self.o.downward),
                self.o.binary('add',pair[1],error[1],self.o.upward))


def partition(x,y,center,radius,boundary):
    lo,hi = center-radius,center+radius
    a,b = x[0],x[-1]
    width = b-a
    candidates = []
    if boundary in (0,3):
        period = width*(2 if boundary==0 else 1)
        for n in range((lo-a)//period-2,(hi-a)//period+3):
            for i in range(len(x)-1):
                candidates.append((x[i]+n*period,x[i+1]+n*period,y[i],y[i+1]))
                if boundary==0:
                    candidates.append((2*b-x[i+1]+n*period,2*b-x[i]+n*period,y[i+1],y[i]))
    else:
        candidates = [(x[i],x[i+1],y[i],y[i+1]) for i in range(len(x)-1)]
        if lo<a:
            candidates.append((lo,a,y[0] if boundary==1 else 0,y[0] if boundary==1 else 0))
        if hi>b:
            candidates.append((b,hi,y[-1] if boundary==1 else 0,y[-1] if boundary==1 else 0))
    result = []
    for start,end,v0,v1 in candidates:
        left,right = max(lo,start),min(hi,end)
        if left>=right:
            continue
        l,r = (left-center)/radius,(right-center)/radius
        slope = (v1-v0)/(end-start)
        result.append((l,r,v0+(center-start)*slope,radius*slope,max(abs(v0),abs(v1))))
    result.sort()
    assert result[0][0]==-1 and result[-1][1]==1
    assert all(p[1]==q[0] for p,q in zip(result,result[1:]))
    return result


def paired(pieces):
    breaks = sorted({F(0),F(1)} | {abs(v) for row in pieces for v in row[:2]})
    value = None
    for lo,hi in zip(breaks,breaks[1:]):
        mid = (lo+hi)/2
        positive = next(row for row in pieces if row[0]<mid<row[1])
        negative = next(row for row in pieces if row[0]<-mid<row[1])
        if positive[3]!=negative[3]:
            return None
        c = positive[2]+negative[2]
        if value is not None and value!=c:
            return None
        value = c
    return value/2


def polynomial(bounds,kernel,radius,parameter,beta,order):
    o,b = bounds.o,bounds
    zero,one = b.rational(0),b.rational(1)
    if kernel==4:
        alpha = radius*radius/(2*parameter*parameter)
        coeff = [F(1)]
        for n in range(1,order+1):
            coeff.append(-coeff[-1]*alpha/n)
        q = alpha/(order+2)
        if q>F(1,2):
            return None
        error = 2*abs(coeff[-1]*alpha/(order+1))
        return [b.rational(v) for v in coeff],b.rational(error)
    frequency = 2*parameter*radius
    if kernel==3:
        big_b = beta*beta/4
        window = [F(0)]*(order+1)
        term = F(1)
        for n in range(order+1):
            for k in range(n+1):
                window[k] += term*math.comb(n,k)*(-1)**k
            term *= big_b/F((n+1)**2)
        if big_b/F((order+2)**2)>F(1,2):
            return None
        window_error = 2*term
        true_bound = sum(big_b**n/F(math.factorial(n)**2) for n in range(order+1))+window_error
        components = [(frequency,window)]
    else:
        coefficients = {0:[F(1,2),F(1,2)],1:[F(27,50),F(23,50)],2:[F(21,50),F(1,2),F(2,25)]}[kernel]
        components = [(frequency,[coefficients[0]])]
        for harmonic,weight in enumerate(coefficients[1:],1):
            for f in (frequency+harmonic,frequency-harmonic):
                components.append((f,[weight*f/(2*frequency)]))
        window_error = F(0)
    result = [zero]*(2*order+1 if kernel==3 else order+1)
    error = zero
    pi = o.pi(o.downward),o.pi(o.upward)
    for frequency,window in components:
        argument = b.multiply(b.multiply(pi,pi),b.rational(frequency*frequency))
        coeff = [one]
        for n in range(1,order+2):
            coeff.append(b.divide(b.negative(b.multiply(coeff[-1],argument)),b.rational((2*n)*(2*n+1))))
        ratio = b.divide(argument,b.rational((2*order+4)*(2*order+5)))
        if o.compare(ratio[1],b.rational(F(1,2))[0])>0:
            return None
        tail = b.multiply(b.rational(2),b.absolute(coeff[-1]))
        if kernel==3:
            contribution = b.add(b.multiply(tail,b.rational(true_bound)),b.rational(window_error))
            contribution = b.add(contribution,b.multiply(tail,b.rational(window_error)))
        else:
            contribution = b.multiply(tail,b.rational(abs(window[0])))
        error = b.add(error,contribution)
        for i in range(order+1):
            for j,weight in enumerate(window):
                result[i+j] = b.add(result[i+j],b.multiply(coeff[i],b.rational(weight)))
    return result,error


def integrate(bounds,coefficients,pieces):
    result = bounds.rational(0)
    for n,coefficient in enumerate(coefficients):
        moment = sum(a*(hi**(2*n+1)-lo**(2*n+1))/(2*n+1)
                     + b*(hi**(2*n+2)-lo**(2*n+2))/(2*n+2)
                     for lo,hi,a,b,_ in pieces)
        result = bounds.add(result,bounds.multiply(coefficient,bounds.rational(moment)))
    return result


def reference(kernel,xt,vt,boundary,radius_raw,parameter_raw,beta_raw,x_raw,y_raw):
    x,y = [number(v,xt) for v in x_raw],[number(v,vt) for v in y_raw]
    if any(not isinstance(v,F) for v in x+y) or any(a>=b for a,b in zip(x,x[1:])):
        return 'domain'
    radius,parameter,beta = (number(v,3) for v in (radius_raw,parameter_raw,beta_raw))
    pieces = [partition(x,y,center,radius,boundary) for center in x]
    results = [None]*len(x)
    for i,p in enumerate(pieces):
        if all(v==y_raw[0] for v in y_raw) and (boundary!=2 or not y_raw[0]):
            results[i] = y_raw[0]
        else:
            exact = paired(p)
            if exact is not None:
                results[i] = ieee_round(exact,32 if vt==4 else 64,False)
                if results[i] is None:
                    return 'overflow'
    for order in (16,32,64,128):
        if all(v is not None for v in results):
            break
        with MPFR(384) as o:
            b = ExactBounds(o)
            generated = polynomial(b,kernel,radius,parameter,beta,order)
            if generated is None:
                continue
            coefficients,error = generated
            denominator = integrate(b,coefficients,[(F(-1),F(1),F(1),F(0),F(1))])
            denominator = b.widen(denominator,b.multiply(b.rational(2),error))
            if o.compare(denominator[0],o.integer(0))<=0:
                continue
            for i,p in enumerate(pieces):
                if results[i] is not None:
                    continue
                numerator = integrate(b,coefficients,p)
                error_scale = sum((hi-lo)*maximum for lo,hi,_,_,maximum in p)
                numerator = b.widen(numerator,b.multiply(b.rational(error_scale),error))
                quotient = b.divide(numerator,denominator)
                low,high = (o.bits(value,vt) for value in quotient)
                if low==high:
                    if number(low,vt) in (math.inf,-math.inf):
                        return 'overflow'
                    results[i]=low
    assert all(v is not None for v in results),(kernel,xt,vt,boundary,radius,parameter,beta,x,y,results)
    return ' '.join(f'{v:x}' for v in results)


def cases():
    for kernel in range(5):
        for xt in (3,4):
            for vt in (3,4):
                for boundary in range(4):
                    x=[bits(v,xt) for v in (F(-1,2),0,F(3,4),2)]
                    y=[bits(v,vt) for v in (1,-2,4,3)]
                    yield kernel,xt,vt,boundary,bits(F(5,4)),bits(F(3,2) if kernel==4 else F(3,4)),bits(2),x,y
        for vt in (3,4):
            for boundary in range(4):
                for y in ([1,1],[3,3],[1<<(31 if vt==4 else 63)]*2):
                    yield kernel,3,vt,boundary,bits(F(1,2)),bits(1 if kernel==4 else F(1,4)),bits(4),[bits(0),bits(1)],y
        for boundary in range(4):
            yield kernel,3,3,boundary,bits(3),bits(1 if kernel==4 else F(1,4)),bits(0),[bits(0),bits(F(1,2)),bits(1)],[bits(v) for v in (0,1,2)]
        # Collinear insertion changes the partition but not the reconstructed function.
        for x,y in (([0,F(3,4),2],[1,F(5,2),5]),([0,F(1,2),F(3,4),1,2],[1,2,F(5,2),3,5])):
            yield kernel,3,3,0,bits(F(1,2)),bits(1 if kernel==4 else F(1,4)),bits(3),[bits(v) for v in x],[bits(v) for v in y]
        # Periodic reconstructed quarter-wave response and a narrow irregular hat.
        yield kernel,3,3,3,bits(2),bits(1 if kernel==4 else F(1,4)),bits(2),[bits(i) for i in range(9)],[bits(v) for v in (0,1,0,-1,0,1,0,-1,0)]
        yield kernel,3,3,2,bits(F(1,8)),bits(1 if kernel==4 else F(3)),bits(2),[bits(v) for v in (0,F(2)**-20,F(1,2),3)],[bits(v) for v in (0,1,0,0)]
        yield kernel,3,3,0,bits(F(1,2)),bits(1 if kernel==4 else F(1,4)),bits(0),[0,bits(1),bits(1)],[0,bits(1),bits(2)]


def main():
    rows=list(cases())
    expected,lines=[],[]
    for row in rows:
        k,xt,vt,boundary,r,p,beta,x,y=row
        expected.append(reference(*row))
        lines.append(f'{k} {xt} {vt} {boundary} {len(x)} {r:x} {p:x} {beta:x} '+' '.join(f'{v:x}' for v in x+y))
    profile=sys.argv[2] if len(sys.argv)>2 else 'strict'
    result=subprocess.run([sys.argv[1],profile,'oracle'],input='\n'.join(lines)+'\n',text=True,capture_output=True,check=True)
    actual=result.stdout.splitlines()
    assert len(actual)==len(expected),(len(actual),len(expected),result.stderr)
    for i,(got,want) in enumerate(zip(actual,expected)):
        assert got==want,(i,rows[i],got,want,result.stderr[:1000])
    print(f'{len(rows)} independent Fraction/directed MPFR continuous lowpass cases passed ({profile})')


if __name__=='__main__':
    main()
