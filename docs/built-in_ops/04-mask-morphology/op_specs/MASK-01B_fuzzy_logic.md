---
spec_schema_version: 1
id: MASK-01B
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-01
function: fuzzy_logic
oracle_entry: fuzzy_logic
registered_operation_keys:
  - mask.fuzzy_logic_strict
---

# MASK-01B: Fuzzy mask operations

Inherit the complete [family contract](MASK-01_boolean_contract.md) and
[MASK-common](MASK_common_contract.md). This Proposed member is registered in the bundled strict CPU set through
fixed invocation ABI 8.

## Ports, parameters and output inference

a,b: Coverage[H,W] -> values: Coverage, same shape/dtype

Required static parameters: operation: String enum and|or|xor|subtract; authoring and. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Use row B, with exact extrema selection and the family tie rule.

E: both masks at Q. Empty Q adds no runtime payload work; static schema checks still apply.
The family defines mathematical Data/Control support, border extension and
rounding stages. Lower the selected complete output tiles to fixed input-tile
reads, using a finite conservative superset for runtime-dependent addressing.
There is no payload Validation read set or runtime dependency discovery.

## Implementation, resource and failure obligations

Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and unchecked numerical premises. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Selected outputs of one call share its publication
outcome; failure exposes no partial call Result. Already retired call exports
remain valid. Preserve host upstream/cancellation/resource status and immutable
owner lifetime.

## Independent acceptance and public workflow

a=b=.5: and=.5, or=.5, xor=0, subtract=.5. Equality is deliberately not ordinary set subtraction.

Independent reference entry: `fuzzy_logic` in
[`mask01/reference.py`](../../../../oracle/ops/mask_morphology/mask01/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Sources and compatibility limits remain in the family and [research notes](../research-sources.md).
