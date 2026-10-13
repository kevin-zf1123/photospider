from __future__ import annotations
from oracle.ops.generation.noi01.rng import block
from oracle.ops.generation.noi01.rng import open52


def poisson_input(x,y,channel=0,**kw):
    b=block(x,y,channel,domain=8,**kw);return open52(b[0],b[1])
