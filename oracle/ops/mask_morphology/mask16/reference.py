from __future__ import annotations
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.reference import dims
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import same
from oracle.ops.mask_morphology.mask06.reference import closing
from oracle.ops.mask_morphology.mask11.reference import flood_barrier
from oracle.ops.mask_morphology.mask11.reference import neighbors
from oracle.ops.mask_morphology.common.registration import op

@op
def bridge_axis_gaps(barrier, maximum_gap, dtype='float64'):
    a = grid(barrier, dtype, 'binary')
    g = integer(maximum_gap, 'maximum_gap')
    h, w = dims(a); out = [[int(v) for v in row] for row in a]
    for line in [[(y, x) for x in range(w)] for y in range(h)]+[[(y, x) for y in range(h)] for x in range(w)]:
        ones = [i for i, (y, x) in enumerate(line) if a[y][x]]
        for left, right in zip(ones, ones[1:]):
            if 0 < right-left-1 <= g:
                for i in range(left+1, right):
                    y, x = line[i]; out[y][x] = 1
    return out
def seed_override_barrier(original, temporary, seeds, dtype):
    a,s=grid(original,dtype,'binary'),grid(seeds,dtype,'binary')
    same(a,s);same(a,temporary)
    filtered = [[float(bool(s[y][x] and not a[y][x])) for x in range(len(a[0]))] for y in range(len(a))]
    effective = [[float(bool(a[y][x] or (temporary[y][x] and not filtered[y][x])))
                  for x in range(len(a[0]))] for y in range(len(a))]
    return effective, filtered

def gap_fill_outputs(barrier, temporary, seeds, fill_connectivity, dtype, request):
    names=('fill','barrier','ignored_seeds','reopened_barrier')
    if request!='all' and request not in names:raise OracleError('invalid gap output request')
    a,s=grid(barrier,dtype,'binary'),grid(seeds,dtype,'binary')
    same(a,s)
    neighbors(0,0,*dims(a),fill_connectivity)
    effective,filtered=seed_override_barrier(a,temporary,s,dtype)
    outputs={}
    if request in ('all','fill'):outputs['fill']=flood_barrier(filtered,effective,fill_connectivity,dtype)
    if request in ('all','barrier'):outputs['barrier']=effective
    if request in ('all','ignored_seeds'):
        outputs['ignored_seeds']=[[float(bool(x and y)) for x,y in zip(ar,sr)] for ar,sr in zip(a,s)]
    if request in ('all','reopened_barrier'):
        outputs['reopened_barrier']=[[float(bool(x and y)) for x,y in zip(tr,sr)] for tr,sr in zip(temporary,filtered)]
    return outputs

@op
def fill_axis_gaps(barrier, seeds, maximum_gap, fill_connectivity=4, dtype='float64', request='all'):
    temporary = bridge_axis_gaps(barrier, maximum_gap, dtype)
    return gap_fill_outputs(barrier,temporary,seeds,fill_connectivity,dtype,request)
@op
def fill_morphological_gaps(barrier, seeds, fill_connectivity=4, footprint='square', radius=None, offsets=None, dtype='float64', request='all'):
    a = grid(barrier, dtype, 'binary')
    c = closing(a, footprint=footprint, radius=radius, offsets=offsets, dtype=dtype)
    temporary = [[int(x or y) for x, y in zip(ar, cr)] for ar, cr in zip(a, c)]
    return gap_fill_outputs(barrier,temporary,seeds,fill_connectivity,dtype,request)
