from __future__ import annotations
from oracle.ops.generation.noi01.rng import block
from oracle.ops.generation.noi01.rng import halfopen53
from oracle.ops.generation.noi01.rng import open52


def gaussian_inputs(x,y,channel=0,domain=2,seed=0,stream=0,frame=0):
    if domain not in (2,10):raise ValueError('Gaussian domain')
    b=block(x,y,channel,domain=domain,seed=seed,stream=stream,frame=frame,draw=0)
    return open52(b[0]),halfopen53(b[1])
