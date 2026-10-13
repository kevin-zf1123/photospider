from __future__ import annotations
from oracle.ops.filter.common.core import DomainError

def block_partition(height,width,payload_height,payload_width,kernel_height,kernel_width,*,save=False):
    if min(height,width,payload_height,payload_width,kernel_height,kernel_width)<1:raise DomainError('block geometry')
    blocks=[]
    for y in range(0,height,payload_height):
        for x in range(0,width,payload_width):
            hh,ww=min(payload_height,height-y),min(payload_width,width-x)
            blocks.append({'payload':(y,x,hh,ww),'logical_read':(y-kernel_height+1,x-kernel_width+1,hh+kernel_height-1,ww+kernel_width-1) if save else (y,x,hh,ww),'discard_prefix':(kernel_height-1,kernel_width-1) if save else (0,0)})
    return blocks
