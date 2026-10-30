"""Independent color equations with certified rounding at model stages.

Matrices and whites are explicit values; the sRGB context is a fixture default.
This module imports no production Photospider implementation.
"""
from pathlib import Path
import csv
from .precision import F, Expr, frac, rn, finite_copy, power, exp, sqrt, compare


def matrix(rows):
    return [[F(str(v)) for v in row] for row in rows]


def dot(a, b):
    return sum((x*y for x, y in zip(a, b)), F(0))


def matvec(m, v):
    return [dot(row, v) for row in m]


def matmul(a, b):
    return [[dot(row, col) for col in zip(*b)] for row in a]


def inverse(m):
    a = [list(map(frac, row)) + [F(i == j) for j in range(3)] for i, row in enumerate(m)]
    for j in range(3):
        pivot = next(i for i in range(j, 3) if a[i][j])
        a[j], a[pivot] = a[pivot], a[j]
        d = a[j][j]
        a[j] = [x/d for x in a[j]]
        for i in range(3):
            if i != j:
                d = a[i][j]
                a[i] = [x-d*y for x, y in zip(a[i], a[j])]
    return [row[3:] for row in a]


def white_xy(x, y):
    x, y = frac(x), frac(y)
    return [x/y, F(1), (1-x-y)/y]


D65 = white_xy(.3127, .3290)
ADAPT = {
    'xyz_scaling': matrix([[1,0,0],[0,1,0],[0,0,1]]),
    'bradford': matrix([['.8951','.2664','-.1614'],['-.7502','1.7135','.0367'],['.0389','-.0685','1.0296']]),
    'cat02': matrix([['.7328','.4296','-.1624'],['-.7036','1.6975','.0061'],['.0030','.0136','.9834']]),
    'cat16': matrix([['.401288','.650173','-.051461'],['-.250268','1.204414','.045854'],['-.002079','.048952','.953127']]),
}
M1 = matrix([['.8190224379967030','.3619062600528904','-.1288737815209879'],
             ['.0329836539323885','.9292868615863434','.0361446663506424'],
             ['.0481771893596242','.2642395317527308','.6335478284694309']])
M2 = matrix([['.2104542683093140','.7936177747023054','-.0040720430116193'],
             ['1.9779985324311684','-2.4285922420485799','.4505937096174110'],
             ['.0259040424655478','.7827717124575296','-.8086757549230774']])


def adaptation(source, target, method='bradford'):
    h = ADAPT[method]
    a, b = matvec(h, source), matvec(h, target)
    return matmul(inverse(h), [[b[i]/a[i]*v for v in row] for i, row in enumerate(h)])


def publish(values, dtype=64):
    return [rn(v, dtype) for v in values]


def root3(x):
    s = compare(x)
    return 0 if s == 0 else s * power(x if s > 0 else -x, F(1,3))


def xyz_lab(xyz, white):
    def f(t):
        return root3(t) if compare(t, F(216,24389)) > 0 else F(841,108)*t+F(4,29)
    fx, fy, fz = [f(frac(x)/w) for x, w in zip(xyz, white)]
    return publish([(116*fy-16)/100,500*(fx-fy),200*(fy-fz)])


def lab_xyz(lab, white, dtype=64):
    l,a,b = map(frac,lab)
    fy = (100*l+16)/116
    def g(t):
        return t**3 if t>F(6,29) else F(108,841)*(t-F(4,29))
    return publish([white[0]*g(fy+a/500),g(fy),white[2]*g(fy-b/200)],dtype)


def xyz_oklab(xyz):
    roots=[root3(x) for x in matvec(M1,list(map(frac,xyz)))]
    return publish(matvec(M2,roots))


def oklab_xyz(lab):
    lms=matvec(inverse(M2),list(map(frac,lab)))
    return publish(matvec(inverse(M1),[x**3 for x in lms]))


class Context:
    """Explicit RGB basis/white for oracle calls, defaulting to sRGB/D65."""
    def __init__(self, rgb_to_xyz=None, white=D65):
        self.white=list(map(frac,white))
        if rgb_to_xyz is None:
            columns=[white_xy(float(x),float(y)) for x,y in [('0.64','0.33'),('0.30','0.60'),('0.15','0.06')]]
            p=list(map(list,zip(*columns)))
            scales=matvec(inverse(p),self.white)
            rgb_to_xyz=[[row[i]*scales[i] for i in range(3)] for row in p]
        self.m=[list(map(frac,row)) for row in rgb_to_xyz]
        self.inv=inverse(self.m)
        self.to_d65=adaptation(self.white,D65)
        self.from_d65=adaptation(D65,self.white)

    def model(self,rgb,route):
        xyz=publish(matvec(self.m,list(map(frac,rgb))))
        if route in ('ratio','xyz_y'):return xyz
        if route=='cielab_l':return xyz_lab(xyz,self.white)
        if route=='oklab_l':
            if self.white!=D65:xyz=publish(matvec(self.to_d65,list(map(frac,xyz))))
            return xyz_oklab(xyz)
        raise ValueError(route)

    def feature(self,rgb,route):
        model=self.model(rgb,route)
        return model[1] if route in ('ratio','xyz_y') else model[0]

    def from_model(self,value,route,dtype=64):
        if route=='cielab_l':value=lab_xyz(value,self.white)
        if route=='oklab_l':
            value=oklab_xyz(value)
            if self.white!=D65:value=publish(matvec(self.from_d65,list(map(frac,value))))
        return publish(matvec(self.inv,list(map(frac,value))),dtype)

    def replace(self,rgb,target,route='ratio',dtype=64):
        model=self.model(rgb,route)
        index=1 if route in ('ratio','xyz_y') else 0
        source=model[index]
        if frac(source)==frac(target):return [finite_copy(x,dtype) for x in rgb]
        if route=='ratio':
            return publish([frac(x)*frac(target)/frac(source) for x in rgb],dtype) if source else publish([frac(target)]*3,dtype)
        model[index]=target
        return self.from_model(model,route,dtype)

    def monochrome(self,rgb,route='ratio',dtype=64):
        model=self.model(rgb,route)
        if route=='ratio':return publish([frac(model[1])]*3,dtype)
        return self.from_model([model[0],0,0],route,dtype)


def white_balance(rgb,source_white,target_white,context=None,destination=None,method='bradford',dtype=64):
    ctx=context or Context(); dst=destination or ctx
    if ctx.m==dst.m and list(source_white)==list(target_white):return [finite_copy(x,dtype) for x in rgb]
    k=matmul(dst.inv,matmul(adaptation(source_white,target_white,method),ctx.m))
    return publish(matvec(k,list(map(frac,rgb))),dtype)


def balanced_sum(values):
    values=list(values)
    while len(values)>1:
        values=[values[i]+values[i+1] if i+1<len(values) else values[i] for i in range(0,len(values),2)]
    return values[0] if values else F(0)


def temperature_white(temperature,tint=0):
    """Strict sampled Planck locus and analytic tangent, then RN64 white."""
    t,d=frac(temperature),frac(tint)
    if not 1000<=t<=20000 or not F('-.03')<=d<=F('.03'):raise ValueError('temperature/tint domain')
    c2=F('6.62607015e-34')*299792458/F('1.380649e-23')
    rows=list(csv.reader((Path(__file__).parent.parent/'data/CIE_xyz_1931_2deg.csv').open()))
    sums=[[],[],[]]; derivatives=[[],[],[]]
    for wavelength,*cmf in rows:
        lam=F(wavelength)*F(1,10**9)
        a=c2/(lam*t); ea=exp(a)
        p=1/(lam**5*(ea-1))
        dp=p*ea*a/(t*(ea-1))
        for i,v in enumerate(cmf):
            sums[i].append(p*F(v)); derivatives[i].append(dp*F(v))
    x,y,z=map(balanced_sum,sums)
    dx,dy,dz=map(balanced_sum,derivatives)
    den=x+15*y+3*z; dd=dx+15*dy+3*dz
    u=4*x/den; v=6*y/den
    if d:
        du=4*(dx*den-x*dd)/(den*den); dv=6*(dy*den-y*dd)/(den*den)
        norm=sqrt(du*du+dv*dv)
        sign=1 if compare(du)>0 else -1
        u=u-sign*d*dv/norm; v=v+sign*d*du/norm
    # Inverse UCS directly to Y-normalized XYZ avoids redundant xy divisions.
    return publish([3*u/(2*v),F(1),(4-u-10*v)/(2*v)])
