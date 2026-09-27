---
spec_schema_version: 1
id: MASK-09A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-09
function: distance_feather_linear
oracle_entry: distance_feather_linear
proposed_operation_keys:
  - mask.distance_feather_linear_strict
---

# MASK-09A: Linear signed-distance feather

Inherit the complete [family contract](MASK-09_distance_feather_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

input: Distance[H,W] -> mask: Coverage, same dtype/shape

Required static parameters: inner,outer>=0 finite Float64; distance_unit,distance_basis, as family. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Result RuntimeCount and association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Use exact family width branches and RN_T(1-u); both-zero is hard d<=0.

E: input[Q]. Empty Q has no runtime sample/control/validation reads. Descriptor
checks still run. The family defines validation closure, border extension,
stage rounding, dynamic witnesses and the forward dirty relation. A bounding
rectangle may not replace a smaller declared sparse support.

## Implementation, resource and failure obligations

Use the family reference algorithm and its stated work/scratch bounds. Strict
CPU is the initial target; no implicit accelerated/GPU/third-party fallback is
registered. Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and finite-domain error rules. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Whole or CompleteBundle members do not publish
partially converged or partially associated results. Preserve the host's
upstream/cancellation/resource scope and immutable owner lifetime.

## Independent acceptance and conceptual workflow

inner=outer=1, d=[-1,0,1] -> [1,.5,0]; inner=outer=0, d=0 ->1.

Independent reference entry: `distance_feather_linear` in
[`reference.py`](../../../../oracle/ops/mask_morphology/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or exact Region reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common ROI/disjoint support, special-value, invalid
parameter, dtype, association, cancellation and owner-lifetime checks before
implementation acceptance.

Conceptual DAG: explicit input preparation -> this member -> an explicit
mask/measurement/image consumer appropriate to the listed output. A real
Compiler/ExecutionContext workflow is still required after registration; this
document does not invent a callable public API. Sources and compatibility
limits are recorded in the family and [research notes](../research-sources.md).

Distance inputs admit signed no-feature infinities and reject NaN. Finite
shift preserves infinities; finite threshold and feather map -Inf to 1 and
+Inf to 0, as applicable. Finite arithmetic overflow remains an error.
