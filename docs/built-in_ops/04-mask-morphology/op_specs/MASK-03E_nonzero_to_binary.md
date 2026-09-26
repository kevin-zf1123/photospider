---
spec_schema_version: 1
id: MASK-03E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-03
function: nonzero_to_binary
oracle_entry: nonzero_to_binary
proposed_operation_keys:
  - mask.nonzero_to_binary_strict
---

# MASK-03E: Explicit UInt8 truthiness adapter

Inherit the complete [family contract](MASK-03_threshold_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

input: UInt8[H,W], raw bytes -> values: Float32/Float64 Binary[H,W]

Required static parameters: output_dtype: float32|float64. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Result RuntimeCount and association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Return 0 iff input byte is 0, otherwise 1. This is the explicit nonzero conversion in the new catalog.

E: input[Q], no color or coverage validation. Empty Q has no runtime sample/control/validation reads. Descriptor
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

[0,1,128,255] -> [0,1,1,1].

Independent reference entry: `nonzero_to_binary` in
[`reference.py`](../../../../examples/mask_morphology_oracle/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or exact Region reads.
Run the [suite commands](../../../../examples/mask_morphology_oracle/README.md),
then add the relevant common ROI/disjoint support, special-value, invalid
parameter, dtype, association, cancellation and owner-lifetime checks before
implementation acceptance.

Conceptual DAG: explicit input preparation -> this member -> an explicit
mask/measurement/image consumer appropriate to the listed output. A real
Compiler/ExecutionContext workflow is still required after registration; this
document does not invent a callable public API. Sources and compatibility
limits are recorded in the family and [research notes](../research-sources.md).
