from __future__ import annotations
from oracle.ops.mask_morphology.common.registration import REGISTRY
from oracle.ops.mask_morphology.common.exact import OracleError



def evaluate(case):
    """Portable oracle case, not WorkflowDocument: {op, inputs, params, dtype}."""
    name = case.get('op')
    if name not in REGISTRY: raise OracleError(f'unknown oracle operation {name}')
    args = dict(case.get('inputs', {})); params = dict(case.get('params', {}))
    if args.keys() & params.keys(): raise OracleError('duplicate input/parameter field')
    args.update(params)
    args.setdefault('dtype', case.get('dtype', 'float64'))
    return REGISTRY[name](**args)


from oracle.ops.mask_morphology.mask01 import reference as _mask01
from oracle.ops.mask_morphology.mask02 import reference as _mask02
from oracle.ops.mask_morphology.mask03 import reference as _mask03
from oracle.ops.mask_morphology.mask04 import reference as _mask04
from oracle.ops.mask_morphology.mask05 import reference as _mask05
from oracle.ops.mask_morphology.mask06 import reference as _mask06
from oracle.ops.mask_morphology.mask07 import reference as _mask07
from oracle.ops.mask_morphology.mask08 import reference as _mask08
from oracle.ops.mask_morphology.mask09 import reference as _mask09
from oracle.ops.mask_morphology.mask10 import reference as _mask10
from oracle.ops.mask_morphology.mask11 import reference as _mask11
from oracle.ops.mask_morphology.mask12 import reference as _mask12
from oracle.ops.mask_morphology.mask13 import reference as _mask13
from oracle.ops.mask_morphology.mask14 import reference as _mask14
from oracle.ops.mask_morphology.mask15 import reference as _mask15
from oracle.ops.mask_morphology.mask16 import reference as _mask16
from oracle.ops.mask_morphology.mask17 import reference as _mask17
