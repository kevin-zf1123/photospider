from __future__ import annotations
from oracle.ops.generation.noi01.rng import block
from oracle.ops.generation.noi01.rng import halfopen53
from oracle.ops.generation.noi01.rng import open52


def gaussian_inputs(x,y,channel=0,domain=2,**kw):
    if domain not in (2,10):raise ValueError('Gaussian domain')
    b=block(x,y,channel,domain=domain,**kw)
    return open52(b[0],b[1]),halfopen53(b[2],b[3])
