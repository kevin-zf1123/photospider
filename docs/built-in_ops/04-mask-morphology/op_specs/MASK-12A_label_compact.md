---
spec_schema_version: 1
id: MASK-12A
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: label_compact
oracle_entry: label_compact
registered_operation_keys:
  - mask.label_compact_strict
---

# MASK-12A: Four/eight-connected compact component labels

Inherit the complete [family contract](MASK-12_components_contract.md) and
[MASK-common](MASK_common_contract.md). This member is registered as
`mask.label_compact_strict` in the bundled strict CPU set through fixed invocation ABI 8.
The Proposed specification status is separate from its implementation status.

## Ports, parameters and output inference

input: Binary[H,W] -> labels: Int64[H,W]

Required static parameters: connectivity 4|8; maximum_count Int64 in [0,N]. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.
The optional `max_work` Int64 operational guard follows MASK-common.

## Mathematics and exact request behavior

Compact first-pixel order, background 0, exact whole-component closure.

complete-input input; labels ROI remains globally numbered. Empty Q adds no runtime payload work; static schema checks still apply.
The family defines mathematical Data/Control support, border extension and
rounding stages. Lower the selected complete output tiles to fixed input-tile
reads, using a finite conservative superset for runtime-dependent addressing.
There is no payload Validation read set or runtime dependency discovery.

## Implementation, resource and failure obligations

Implemented in [`components.cpp`](../../../../plugins/ops/04-mask-morphology/components.cpp).
The family records the algorithm, admitted scratch and complete publication
contract. Strict CPU is registered; no implicit GPU/third-party fallback is
registered. Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and unchecked numerical premises. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Selected outputs of one call share its publication
outcome; failure exposes no partial call Result. Already retired call exports
remain valid. Preserve host upstream/cancellation/resource status and immutable
owner lifetime.

## Independent acceptance and conceptual workflow

binary [[1,0],[0,1]], connectivity4 -> [[1,0],[0,2]]; connectivity8 -> [[1,0],[0,1]].

Independent reference entry: `label_compact` in
[`mask12/reference.py`](../../../../oracle/ops/mask_morphology/mask12/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Conceptual DAG: explicit input preparation -> this member -> an explicit
mask/measurement/image consumer appropriate to the listed output. Use the registered key in a `WorkflowDocument`, compile with `Compiler`,
then prepare and submit through `ExecutionContext`. Read the declared output
port from the retained Result after `RunHandle::join`. Sources and compatibility
limits are recorded in the family and [research notes](../research-sources.md).
