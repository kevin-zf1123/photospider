---
spec_schema_version: 1
id: MASK-04C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-04
function: hue_range
oracle_entry: hue_range
proposed_operation_keys:
  - mask.hue_range_strict
---

# MASK-04C: Periodic hue selector with explicit neutral policy

Inherit the complete [family contract](MASK-04_color_range_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

image: Field[C,H,W] -> mask: Coverage[H,W]

Required static parameters: channels selects hue,chroma; optional input_description; center_turns finite; minimum_chroma>=0 finite; neutral_policy include|exclude; inner,outer in [0,.5]; curve. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Use exact turn reduction. A sample is neutral when chroma is exactly zero or
strictly below minimum_chroma. The explicit neutral_policy returns 1 for include
and 0 for exclude; the authoring recommendation is exclude. Equality to a positive
minimum_chroma uses the hue branch. Selected-coordinate domains are unchecked
premises; unused polar coordinates add no reads.

E hue/chroma Data only; no spatial halo. Empty Q adds no runtime payload work; static schema checks still apply.
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

hue=[0,1,2,-1], chroma=1, center=0, inner=outer=0 -> [1,1,1,1]; neutral exclusion gives 0.

Independent reference entry: `hue_range` in
[`mask04/reference.py`](../../../../oracle/ops/mask_morphology/mask04/reference.py).
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
