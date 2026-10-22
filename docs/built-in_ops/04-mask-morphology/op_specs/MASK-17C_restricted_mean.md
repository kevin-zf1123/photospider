---
spec_schema_version: 1
id: MASK-17C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-17
function: restricted_mean
oracle_entry: restricted_mean
proposed_operation_keys:
  - mask.restricted_mean_strict
---

# MASK-17C: Mask-restricted local mean with denominator validity

Inherit the complete [family contract](MASK-17_apply_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

image: Float32/64[C,H,W], mask: same-dtype Coverage[H,W] -> values: Float32/64[C,H,W], valid: Binary[H,W]

Required static parameters: MASK-05 footprint schema; empty_policy zero|error. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Exact all-sample weighted numerator and weight-sum denominator; explicit zero-denominator behavior on the finite input domain.

H mask support S; all image samples in S for requested value channels, including zero weights; valid alone has no image Data. Empty Q adds no runtime payload work; static schema checks still apply.
The family defines mathematical Data/Control support, border extension and
rounding stages. Lower the selected complete output tiles to fixed input-tile
reads, using a finite conservative superset for runtime-dependent addressing.
There is no payload Validation read set or runtime dependency discovery.

## Implementation, resource and failure obligations

Use the family reference algorithm and its stated work/scratch bounds. Strict
CPU is the initial target; no implicit accelerated/GPU/third-party fallback is
registered. Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and unchecked numerical premises. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Selected outputs of one call share its publication
outcome; failure exposes no partial call Result. Already retired call exports
remain valid. Preserve host upstream/cancellation/resource status and immutable
owner lifetime.

## Independent acceptance and conceptual workflow

image=[0,10], weights=[1,0] and a footprint covering both ->0, not5; weights[1,1] ->5; allzero ->valid0.

Independent reference entry: `restricted_mean` in
[`mask17/reference.py`](../../../../oracle/ops/mask_morphology/mask17/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Conceptual DAG: explicit input preparation -> this member -> an explicit
mask/measurement/image consumer appropriate to the listed output. A real
Compiler/ExecutionContext workflow is still required after registration; this
document does not invent a callable public API. Sources and compatibility
limits are recorded in the family and [research notes](../research-sources.md).

Binary flags use the associated value output floating dtype and exact 0/1.
