---
spec_schema_version: 1
id: MASK-07D
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-07
function: offset_polygon_grid
oracle_entry: offset_polygon_grid
proposed_operation_keys:
  - mask.offset_polygon_grid_strict
---

# MASK-07D: Continuous polygon offset with fixed sample-grid coverage

Inherit the complete [family contract](MASK-07_offset_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

vertices: Field[V,2] -> mask: Coverage[H,W]

Required static parameters: H,W; sy,sx; radius; sample_pattern; output_dtype; grid_center requires samples_per_axis, vulkan_standard requires sample_count, as family. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Result RuntimeCount and association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Use exact signed point-to-polygon distance membership at the selected profile's N sample points; final rational count rounding.

Whole vertices and polygon validation for any nonempty Q; output pixels independently evaluable. Empty Q has no runtime sample/control/validation reads. Descriptor
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

polygon cell corners (-.5,-.5),(-.5,.5),(.5,.5),(.5,-.5), H=W=1,n=2: r=0 ->1; r=-.3 ->0.

Independent reference entry: `offset_polygon_grid` in
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
