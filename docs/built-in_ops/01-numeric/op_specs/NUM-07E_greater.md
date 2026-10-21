---
spec_schema_version: 1
id: NUM-07E
parent_id: NUM-07
function: greater
proposed_operation_keys:
  - numeric.greater_strict
  - numeric.greater_accelerated_apple_silicon
  - numeric.greater_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07E: greater

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

For each coordinate in a requested output tile, return UInt8 1 when `a > b` is true, otherwise
UInt8 0. Inputs `a` and `b` have identical shape and dtype (UInt8, Int64, Float32
or Float64). Output `values` has that shape and no attached domain description. No static numeric
parameters or implicit cast/broadcast is provided.

Inherit the [exact comparison contract](NUM-07_comparison_contract.md) in full:
floating numeric relations, NaN behavior, signed-zero equality, exact integer
comparison, bitwise equivalence of the three CPU profiles, fixed two-port tile support, structural preparation, returned storage, resources, errors and lifetime requirements. This is numerical comparison, not bit-pattern comparison.

## Acceptance and implementation status

Conceptual public fixture: a=[1,2,3], b=[2,2,2] -> [0,0,1]. Bind both
arrays in WorkflowDocument, compile the selected key and read UInt8 values
through ExecutionContext. Repeat all supported dtypes and valid platform keys.
Use independent exact relation/bit classification rather than the production
comparison helper. Include Int64 extrema and values above 2^53, signed zeros,
subnormal neighbors, infinities, signaling/quiet NaNs in either input position
and the shared disjoint/strided/resource/structural-validation acceptance cases.

All three profile definitions lower to fixed tile tasks. Temporary acceptance should check declared input support, special bits, signed strides, cancellation/resources and composition with select. The specification remains Proposed; these requirements do not report a test or performance result.

See [NUM-07 fixed execution](../comparison-execution.md) for invocation, capacity and validation boundaries.
