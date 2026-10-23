---
spec_schema_version: 1
id: MASK-12F
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-12
function: component_bundle
oracle_entry: component_bundle
registered_operation_keys:
  - mask.component_bundle_strict
---

# MASK-12F: MinPixel labels with complete component attributes

Inherit the complete [family contract](MASK-12_components_contract.md) and
[MASK-common](MASK_common_contract.md). This member is registered as
`mask.component_bundle_strict` in the bundled strict CPU set through fixed invocation ABI 8.
The Proposed specification status is separate from its implementation status.

## Ports, parameters and output inference

input: Binary[H,W] -> components: complete Result labels:Int64[H,W], rows:Int64[N,7] plus count:Int64[1], N=H*W

Required static parameters: connectivity 4|8; maximum_count in [0,N]. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.
The optional `max_work` Int64 operational guard follows MASK-common.

## Mathematics and exact request behavior

B labels plus exact area,min,bbox columns as family; labels and table share one source identity.

complete-input input; one fixed multi-member Result with count, rows and labels. Empty Q adds no runtime payload work; static schema checks still apply.
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

single foreground at row-major index3 in 2x2 -> id4,area1,min3,bbox[1,1,2,2].

Independent reference entry: `component_bundle` in
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
