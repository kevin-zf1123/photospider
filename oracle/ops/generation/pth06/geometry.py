from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.geometry import cross
from oracle.ops.generation.common.geometry import point


def affine_transform(points,matrix):
    if len(matrix)!=2 or any(len(row)!=3 for row in matrix):raise ValueError('2x3 matrix')
    A=[list(map(frac,row)) for row in matrix]
    if A==[[F(1),F(0),F(0)],[F(0),F(1),F(0)]]:return [tuple(p) for p in points]
    return [tuple(round_fraction(A[j][0]*frac(p[0])+A[j][1]*frac(p[1])+A[j][2]) for j in range(2)) for p in points]


def polygon_area(contour):
    p=list(map(point,contour))
    return sum((cross(a,b) for a,b in zip(p,p[1:]+p[:1])),F(0))/2 if p else F(0)
