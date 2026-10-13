"""Directed-MPFR certified local transcendental references, with explicit caps."""

from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import exp_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac

def gaussian_kernel(sigma,radius):
    s=frac(sigma)
    if s<0 or not isinstance(radius,int) or radius<0 or (s==0 and radius!=0):raise DomainError('Gaussian sigma/radius')
    if radius>256:raise DomainError('oracle fixture cap radius=256; not a production spec limit')
    if not s:return [1.0]
    a={j:exp_q(-Q(j*j)/(2*s*s)) for j in range(radius+1)}
    return [a[abs(j)] for j in range(-radius,radius+1)]
