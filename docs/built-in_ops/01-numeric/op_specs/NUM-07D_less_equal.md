---
spec_schema_version: 1
id: NUM-07D
parent_id: NUM-07
function: less_equal
proposed_operation_keys:
  - numeric.less_equal_strict
  - numeric.less_equal_accelerated_apple_silicon
  - numeric.less_equal_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-07D: less_equal

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

For each requested coordinate, return UInt8 1 when `a <= b` is true, otherwise
UInt8 0. Inputs `a` and `b` have identical shape and dtype (UInt8, Int64, Float32
or Float64). Output `values` has that shape and empty facets. No static numeric
parameters or implicit cast/broadcast is provided.

Inherit the [exact comparison contract](NUM-07_comparison_contract.md) in full:
floating numeric relations, NaN behavior, signed-zero equality, exact integer
comparison, bitwise equivalence of the three CPU profiles, two-port Q support,
typed validation, invalidation, returned storage, resource/error and lifetime
requirements. This is numerical comparison, not bit-pattern comparison.

## Acceptance and implementation status

Conceptual public fixture: a=[1,2,3], b=[2,2,2] -> [1,1,0]. Bind both
arrays in WorkflowDocument, compile the selected key and read UInt8 values
through ExecutionContext. Repeat all supported dtypes and valid platform keys.
Use independent exact relation/bit classification rather than the production
comparison helper. Include Int64 extrema and values above 2^53, signed zeros,
subnormal neighbors, infinities, signaling/quiet NaNs in either input position
and the shared disjoint/strided/resource/typed-validation acceptance cases.

The proposed keys are not implemented and no public runtime test is claimed.
