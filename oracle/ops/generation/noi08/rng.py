from __future__ import annotations
from oracle.ops.generation.noi01.rng import block
from oracle.ops.generation.noi01.rng import open52


def poisson_input(x,y,channel=0,seed=0,stream=0,frame=0):
    b=block(x,y,channel,domain=8,seed=seed,stream=stream,frame=frame,draw=0);return open52(b[0])
