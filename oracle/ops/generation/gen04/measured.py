from __future__ import annotations
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def zone_plate(p,center,k,phase=0,lo=0,hi=1,dps=80):
    with mp.workdps(dps):
        r2=sum((_mp(a)-_mp(b))**2 for a,b in zip(p,center))
        return +(_mp(lo)+(_mp(hi)-_mp(lo))*(1+mp.cos(2*mp.pi*_mp(k)*r2+_mp(phase)))/2)


def siemens_star(p,center,spokes,phase=0,lo=0,hi=1,dps=80):
    if not isinstance(spokes,int) or spokes<1:raise ValueError('spokes')
    with mp.workdps(dps):
        x,y=[_mp(a)-_mp(b) for a,b in zip(p,center)]
        if x==0 and y==0:return +((_mp(lo)+_mp(hi))/2)
        return +(_mp(lo)+(_mp(hi)-_mp(lo))*(1+mp.cos(spokes*mp.atan2(y,x)+_mp(phase)))/2)
