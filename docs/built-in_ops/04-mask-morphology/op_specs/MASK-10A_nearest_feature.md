---
spec_schema_version: 1
id: MASK-10A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-10
function: nearest_feature
oracle_entry: nearest_feature
proposed_operation_keys:
  - mask.nearest_feature_strict
---

# MASK-10A: Nearest discrete feature and distance

Inherit the complete [family contract](MASK-10_distance_transform_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

input: Binary[H,W] -> distance: Float32/64[H,W], nearest: Int64[2,H,W]

Required static parameters: metric,sy,sx,output_dtype,exterior,feature,squared, as family. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

Authoring recommendation: exterior=none. Both none and background are
supported; the node must explicitly serialize exterior.

## Mathematics and exact request behavior

Exact nearest site and lexicographic tie; distance is RN_T(metric distance), or RN_T(squared L2); explicit no-feature +Inf.

complete-input Binary input for any nonempty output demand; buffers are separately requestable, shared site choice. Empty Q adds no runtime payload work; static schema checks still apply.
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

input=[1,0,1], feature=foreground,exterior=none,l2 -> distances=[0,1,0]; middle nearest=(0,0). All-zero foreground search gives +Inf and nearest=(-1,-1).

Independent reference entry: `nearest_feature` in
[`mask10/reference.py`](../../../../oracle/ops/mask_morphology/mask10/reference.py).
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

No-feature results use the family infinity rule. No valid output is present.
