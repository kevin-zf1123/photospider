from __future__ import annotations
from collections import deque
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import format_info
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.reference import blank
from oracle.ops.mask_morphology.common.reference import coords
from oracle.ops.mask_morphology.common.reference import dims
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import labels_grid
from oracle.ops.mask_morphology.mask11.reference import neighbors
from oracle.ops.mask_morphology.common.registration import op

def components(a, connectivity):
    h, w = dims(a)
    neighbors(0, 0, h, w, connectivity)
    unseen = {(y, x) for y, x in coords(a) if a[y][x]}
    groups = []
    while unseen:
        seed = min(unseen)
        unseen.remove(seed); queue = deque([seed]); group = [seed]
        while queue:
            y, x = queue.popleft()
            for site in neighbors(y, x, h, w, connectivity):
                if site in unseen:
                    unseen.remove(site); queue.append(site); group.append(site)
        groups.append(sorted(group))
    return groups

def labeling(input, connectivity, maximum_count, dtype, min_pixel):
    a = grid(input, dtype, "binary")
    h, w = dims(a); maximum_count = h*w if maximum_count is None else integer(maximum_count, "maximum_count", 0, h*w)
    groups = components(a, connectivity)
    if len(groups) > maximum_count: raise OracleError("component count exceeds declared domain cap")
    out = blank(a, 0)
    for i, group in enumerate(groups, 1):
        label = 1+group[0][0]*w+group[0][1] if min_pixel else i
        for y, x in group: out[y][x] = label
    return out

@op
def label_compact(input, connectivity=4, maximum_count=None, dtype="float64"):
    return labeling(input, connectivity, maximum_count, dtype, False)
@op
def label_min_pixel(input, connectivity=4, maximum_count=None, dtype="float64"):
    return labeling(input, connectivity, maximum_count, dtype, True)

def label_groups(labels):
    a = labels_grid(labels)
    groups = {}
    for y, x in coords(a):
        if a[y][x]: groups.setdefault(a[y][x], []).append((y, x))
    return a, sorted(groups.items())

@op
def component_count(labels, dtype=None):
    return [len(label_groups(labels)[1])]
@op
def component_areas(labels, dtype=None):
    return {"rows": [[id, len(sites)] for id, sites in label_groups(labels)[1]]}
@op
def component_bboxes(labels, dtype=None):
    return {"rows": [[id, min(y for y, x in sites), min(x for y, x in sites),
                      max(y for y, x in sites)+1, max(x for y, x in sites)+1]
                     for id, sites in label_groups(labels)[1]]}
@op
def component_bundle(input, connectivity=4, maximum_count=None, dtype="float64"):
    labels = label_min_pixel(input, connectivity, maximum_count, dtype)
    _, groups = label_groups(labels)
    return {"labels": [v for row in labels for v in row],
            "rows": [[id, len(sites), id-1, min(y for y, x in sites), min(x for y, x in sites),
                       max(y for y, x in sites)+1, max(x for y, x in sites)+1] for id, sites in groups]}
@op
def filter_area_index(labels, area_index, minimum_area, labels_id, output_dtype="float64", dtype=None):
    format_info(output_dtype)
    a, _ = label_groups(labels)
    minimum_area = integer(minimum_area, "minimum_area", 1)
    if not isinstance(area_index, dict) or not isinstance(labels_id, str) or not labels_id or area_index.get('source_id') != labels_id:
        raise OracleError("source association mismatch", "InvalidAssociation")
    expected = component_areas(labels)['rows']
    rows = area_index.get('rows')
    if not isinstance(rows, list) or any(not isinstance(r, list) or len(r) != 2 for r in rows):
        raise OracleError('area rows must be Kx2', 'TypeMismatch')
    for row in rows:
        for value in row: integer(value, 'area_index', 1)
    if rows != expected: raise OracleError("area rows incomplete, unsorted or incorrect", "InvalidAssociation")
    areas = dict(rows)
    return [[rn(int(v != 0 and areas[v] >= minimum_area), output_dtype) for v in row] for row in a]
