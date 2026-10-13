from __future__ import annotations
from oracle.ops.mask_morphology.mask05.reference import morph
from oracle.ops.mask_morphology.common.registration import op
@op
def opening(input, **kw): return morph(input, "opening", **kw)
@op
def closing(input, **kw): return morph(input, "closing", **kw)
