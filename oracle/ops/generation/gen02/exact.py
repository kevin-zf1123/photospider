from __future__ import annotations
from fractions import Fraction as F


def coordinate(x, y, width, height, origin=(0,0), normalized=False):
    if not (width >= 1 and height >= 1 and 0 <= x < width and 0 <= y < height):
        raise ValueError('invalid canvas/index')
    if normalized:
        return F(2*x+1,2*width), F(2*y+1,2*height)
    return F(2*(origin[0]+x)+1,2), F(2*(origin[1]+y)+1,2)


# Stable descriptive aliases used by the draft specs (same helper semantics).
coordinates=coordinate
