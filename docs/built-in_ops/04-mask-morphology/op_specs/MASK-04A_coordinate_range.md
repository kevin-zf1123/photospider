---
spec_schema_version: 1
id: MASK-04A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: coordinate_range
oracle_entry: coordinate_range
registered_operation_keys:
  - mask.coordinate_range_strict
---

# MASK-04A: Scaled coordinate-distance mask

Inherit the complete [family contract](MASK-04_color_range_contract.md) and
[MASK-common](MASK_common_contract.md). This Proposed member is registered in the bundled strict CPU set through
fixed invocation ABI 8.

## Ports, parameters and output inference

image: Field[C,H,W], target: Float64[K], scales: Float64[K] -> values: Coverage[H,W]

Required static parameters: channels; optional input_description; inner,outer; curve, as family. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Use scaled Euclidean distance and the exact family radial ramp.

E image selected components; no complete-group payload scan. complete-input target/scales control arrays for any nonempty Q. Empty Q adds no runtime payload work; static schema checks still apply.
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

single coordinate image=[0,.5,1], target=[0], scales=[1], inner=0,outer=1,linear -> [1,.5,0].

Independent reference entry: `coordinate_range` in
[`mask04/reference.py`](../../../../oracle/ops/mask_morphology/mask04/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Sources and compatibility limits remain in the family and [research notes](../research-sources.md).
