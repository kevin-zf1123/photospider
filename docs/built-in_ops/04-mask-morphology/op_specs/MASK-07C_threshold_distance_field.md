---
spec_schema_version: 1
id: MASK-07C
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-07
function: threshold_distance_field
oracle_entry: threshold_distance_field
registered_operation_keys:
  - mask.threshold_distance_field_strict
---

# MASK-07C: Threshold a signed distance/level set

Inherit the complete [family contract](MASK-07_offset_contract.md) and
[MASK-common](MASK_common_contract.md). This Proposed member is registered in the bundled strict CPU set through
fixed invocation ABI 8.

## Ports, parameters and output inference

input: Distance[H,W] -> values: floating Binary[H,W]

Required static parameters: radius signed finite Float64; distance_unit and distance_basis as B. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Exact comparison d<=radius; output 1 or +0.

E: input[Q]. Empty Q adds no runtime payload work; static schema checks still apply.
The family defines mathematical Data/Control support, border extension and
rounding stages. Lower the selected complete output tiles to fixed input-tile
reads, using a finite conservative superset for runtime-dependent addressing.
There is no payload Validation read set or runtime dependency discovery.

## Implementation, resource and failure obligations

The native implementation is in [distance_fields.cpp](../../../../plugins/ops/04-mask-morphology/distance_fields.cpp);
its algorithm, scratch and strict-path limits are described in the family.
Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and unchecked numerical premises. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Selected outputs of one call share its publication
outcome; failure exposes no partial call Result. Already retired call exports
remain valid. Preserve host upstream/cancellation/resource status and immutable
owner lifetime.

## Independent acceptance and public workflow

d=[-.5,0,.5], radius=0 -> [1,1,0].

Independent reference entry: `threshold_distance_field` in
[`mask07/reference.py`](../../../../oracle/ops/mask_morphology/mask07/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Use the registered operation key above with explicit static parameters and
input Results in the listed port order. Compile with `Compiler`, then execute
with `ExecutionContext::prepare`, `submit` and `RunHandle::join`. Select the
listed output keys; partial requests retain global output coordinates.
Sources and compatibility limits remain in the family and
[research notes](../research-sources.md).

Distance inputs admit signed no-feature infinities and exclude NaN as a caller
premise. Finite shift preserves infinities; finite threshold and feather map
-Inf to 1 and +Inf to 0, as applicable. Finite results must be representable;
violated numerical premises have no runtime diagnostic guarantee.
