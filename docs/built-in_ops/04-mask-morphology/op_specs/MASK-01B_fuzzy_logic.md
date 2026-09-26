---
spec_schema_version: 1
id: MASK-01B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-01
function: fuzzy_logic
oracle_entry: fuzzy_logic
proposed_operation_keys:
  - mask.fuzzy_logic_strict
---

# MASK-01B: Fuzzy mask operations

Inherit the complete [family contract](MASK-01_boolean_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

a,b: Coverage[H,W] -> values: Coverage, same shape/dtype

Required static parameters: operation: String enum and|or|xor|subtract; authoring and. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Result RuntimeCount and association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Use row B, with exact extrema selection and the family tie rule.

E: both masks at Q. Empty Q has no runtime sample/control/validation reads. Descriptor
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

a=b=.5: and=.5, or=.5, xor=0, subtract=.5. Equality is deliberately not ordinary set subtraction.

Independent reference entry: `fuzzy_logic` in
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
