from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.rng import MASK
from oracle.ops.generation.common.rng import MASK32
from oracle.ops.generation.common.rng import _integer


def philox4x64(counter,key,rounds=10):
    if rounds!=10:raise ValueError('this contract freezes exactly ten rounds')
    if len(counter)!=4 or len(key)!=2:raise ValueError('counter/key width')
    c=[_integer(v,0,MASK,'counter word') for v in counter]
    k=[_integer(v,0,MASK,'key word') for v in key]
    for r in range(10):
        p0=0xD2E7470EE14C6C93*c[0];p1=0xCA5A826395121157*c[2]
        c=[((p1>>64)^c[1]^k[0])&MASK,p1&MASK,((p0>>64)^c[3]^k[1])&MASK,p0&MASK]
        if r!=9:k=[(k[0]+0x9E3779B97F4A7C15)&MASK,(k[1]+0xBB67AE8584CAA73B)&MASK]
    return tuple(c)


def pack_counter(x,y,channel=0,domain=1,stream=0,frame=0,draw=0):
    _integer(x,-(1<<31),(1<<31)-1,'x');_integer(y,-(1<<31),(1<<31)-1,'y')
    _integer(channel,0,65535,'channel');_integer(domain,0,255,'domain')
    _integer(stream,0,MASK32,'stream');_integer(frame,0,MASK32,'frame');_integer(draw,0,MASK32,'draw')
    return (x&MASK32)|((y&MASK32)<<32),frame|(draw<<32),stream|(channel<<32)|(domain<<48),0


def block(x,y,channel=0,domain=1,seed=0,stream=0,frame=0,draw=0):
    _integer(seed,-(1<<63),(1<<63)-1,'seed')
    s=seed&((1<<64)-1)
    return philox4x64(pack_counter(x,y,channel,domain,stream,frame,draw),(s,0))


def halfopen53(a,b):return F(((a>>37)<<26)|(b>>38),1<<53)
def open52(a,b):return F(2*(((a>>38)<<26)|(b>>38))+1,1<<53)

def uniform(x,y,channel=0,dtype='float64',**kw):
    b=block(x,y,channel,domain=1,**kw)
    if dtype=='float32':return float(F(b[0]>>40,1<<24))
    if dtype=='float64':return float(halfopen53(b[0],b[1]))
    raise ValueError('dtype')
