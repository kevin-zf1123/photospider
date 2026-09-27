"""Public WorkflowDocument acceptance against independent exact/MPFR oracles."""
import random
import struct
import subprocess
import sys
from elementary_oracle import reference as elementary
from certified_oracle import reference as mathematical
from reduction_oracle import format_info
from math_oracle_support import MPFR

BASIC = {'abs':0,'neg':1,'sqrt':2,'floor':3,'ceil':4,'round':5,'sign':6,'reciprocal':7}
MATH = {'exp':0,'ln':1,'sin':2,'cos':3,'tan':4,'sinpi':5,'cospi':6,'tanpi':7,'sinc':8,'sincpi':9,
        'sinpi_rational':13,'cospi_rational':14,'tanpi_rational':15,'sincpi_rational':16}


def cases():
    rng = random.Random(404)
    for operation in ('abs','floor','ceil','round','sign'):
        for raw in range(256):
            yield operation,1,1,raw,0
    for operation in ('abs','neg','floor','ceil','round','sign'):
        for raw in [0,1,(1<<64)-1,1<<63,(1<<63)-1,(1<<53)+1]+[rng.getrandbits(64) for _ in range(74)]:
            yield operation,2,2,raw,0
    for dtype in (3,4):
        p,bias,width = format_info(dtype)
        sign = 1 << (width-1)
        infinity = ((1 << (width-p-1))-1) << p
        bits = lambda value: int.from_bytes(struct.pack('>f' if dtype == 4 else '>d',value),'big')
        pool = [0,sign,1,sign|1,(1<<p)-1,1<<p,(1<<p)+1,infinity-1,sign|(infinity-1),
                infinity,sign|infinity,infinity|0x42,sign|infinity|0x13,infinity|(1<<(p-1))|0x51]
        for value in (.25,.5,.75,1,1.5,2,3,10,100,-.25,-.5,-.75,-1,-1.5,-2):
            raw = bits(value)
            pool += [raw-1,raw,raw+1]
        for operation in list(BASIC)+[name for name in MATH if not name.endswith('_rational')]:
            for raw in pool+[rng.getrandbits(width) for _ in range(70)]:
                yield operation,dtype,dtype,raw,0
        ratios = [(0,1),(1,1),(-1,1),(1,2),(1,3),(1,4),(1,6),(5,6),(-5,6),(3,4),(7,4),
                  (2,3),(4,3),(-1,3),((1<<53)+1,1),((1<<53)+1,2),(-(1<<63),(1<<63)-1),
                  ((1<<63)-1,7),(1,(1<<63)-1),(0,0),(1,-1)]
        ratios += [(n,d) for d in (1,2,3,4,6) for n in range(-2*d,2*d+1)]
        ratios += [(rng.randrange(-(1<<63),1<<63),rng.randrange(1,1<<63)) for _ in range(50)]
        for operation in ('sinpi_rational','cospi_rational','tanpi_rational','sincpi_rational'):
            for n,d in ratios:
                yield operation,2,dtype,n%(1<<64),d%(1<<64)


from accuracy_oracle import accepted

def main():
    rows = list(cases())
    encoded,wanted = [],[]
    for operation,source,destination,a,b in rows:
        encoded.append(f'{operation} {source} {destination} {a:x} {b:x}')
        value = elementary(BASIC[operation],source,a,b) if operation in BASIC else mathematical(MATH[operation],destination,a,b)
        wanted.append('denominator' if value == 'invalid' else value if isinstance(value,str) else f'{value:x}')
    result = subprocess.run([sys.argv[1],sys.argv[2] if len(sys.argv)>2 else 'strict','oracle'],
                            input='\n'.join(encoded)+'\n',capture_output=True,text=True,check=True)
    answers = result.stdout.splitlines()
    assert len(answers) == len(rows),(len(answers),len(rows),result.stderr)
    for index,(actual,expected) in enumerate(zip(answers,wanted)):
        assert accepted(actual, expected, rows[index][2], sys.argv[2] if len(sys.argv)>2 else "strict"),(index,rows[index],actual,expected)
    with MPFR(128) as oracle:
        version = oracle.version
    print(f'{len(rows)} independent exact/MPFR-{version} unary cases passed ({sys.argv[2] if len(sys.argv)>2 else "strict"})')


if __name__ == '__main__':
    main()
