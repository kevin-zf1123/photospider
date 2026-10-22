---
spec_schema_version: 1
id: MASK-17A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-17
function: affect_result
oracle_entry: affect_result
registered_operation_keys:
  - mask.affect_result_strict
---

# MASK-17A: Interpolate a processed result by a mask

Inherit the complete [family contract](MASK-17_apply_contract.md) and
[MASK-common](MASK_common_contract.md). This Proposed member is registered in the bundled strict CPU set through
fixed invocation ABI 8.

## Ports, parameters and output inference

original,processed: Float32/64[C,H,W], mask: same-dtype Coverage[H,W] -> values: image dtype/shape

Required static parameters: channels: ordered unique canonical String; no implicit alpha/color conversion. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Selected channels always use RN_T((1-m)*a+m*b) under finite-operand premises,
including m=0/1; unselected channels copy original.

E: both selected operands and mask at Q, independent of weight. Empty Q adds no runtime payload work; static schema checks still apply.
The family defines mathematical Data/Control support, border extension and
rounding stages. Lower the selected complete output tiles to fixed input-tile
reads, using a finite conservative superset for runtime-dependent addressing.
There is no payload Validation read set or runtime dependency discovery.

## Implementation, resource and failure obligations

Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and unchecked numerical premises. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Selected outputs of one call share its publication
outcome; failure exposes no partial call Result. Already retired call exports
remain valid. Preserve host upstream/cancellation/resource status and immutable
owner lifetime.

## Independent acceptance and public workflow

original=2,processed=10,m=.25 ->4; finite operands at m=0/1 preserve the stated
expression. Nonfinite operand fixtures exercise oracle diagnostics only.

Independent reference entry: `affect_result` in
[`mask17/reference.py`](../../../../oracle/ops/mask_morphology/mask17/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Sources and compatibility limits remain in the family and [research notes](../research-sources.md).
