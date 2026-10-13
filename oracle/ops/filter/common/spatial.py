"""Small 2D-plane references for FIL. Batch/channel lifting is intentionally external.

Each public function evaluates the full requested small array; support_for_output
independently reports a point's exact source set. This is not a tile execution
engine, cancellation test, or runtime metadata/ownership implementation.
"""

from __future__ import annotations
from oracle.ops.filter.common.core import coordinate
from oracle.ops.filter.common.core import shape

def window_samples(image,y,x,height,width,anchor,boundary,cval=0,footprint=None):
    values=[]
    for j in range(height):
        for i in range(width):
            if footprint is not None and not footprint[j][i]:continue
            yy,xx=y+j-anchor[0],x+i-anchor[1]
            h,w=shape(image);p=coordinate(yy,xx,h,w,boundary)
            if p is None:
                if boundary=='truncate':continue
                values.append(float(cval))
            else:values.append(image[p[0]][p[1]])
    return values
