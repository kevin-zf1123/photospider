---
spec_schema_version: 1
id: NUM-05F
parent_id: NUM-05
function: maximum
proposed_operation_keys:
  - numeric.maximum_strict
  - numeric.maximum_accelerated_apple_silicon
  - numeric.maximum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-05F: maximum

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Return the greater value of matching elements in inputs `a` and `b`.
Support UInt8, Int64, Float32 and Float64; shapes and dtypes match exactly.
Output `values` preserves shape and dtype with empty facets. There are no static
numeric parameters. Inherit the [binary contract](NUM-05_binary_contract.md)
for both-operand demand, typed validation, errors, resources and execution.

## Numeric semantics

All three profiles are bitwise identical. Integer comparison preserves the
selected value exactly, including Int64 extrema, without conversion to floating
point. Floating infinities participate in the ordinary numerical order.

If either operand is NaN, propagate that input's payload/sign and quiet it.
If both are NaN, propagate the first input `a` under the shared priority rule.
This is not a NaN-ignoring operation. Both source operands are still read and
validated, even if a NaN determines the output.

For mixed signed zeros, return +0. For same-sign zeros, retain that sign.
For equal nonzero operands, return their common value. These exact selection
rules introduce no rounding tolerance or numeric overflow failure.

## Acceptance and current implementation

Bind a=[-2,0,5] and b=[1,0,3] through a public WorkflowDocument and inspect the
expected values [1,0,5] in both ordinary and disjoint requests. Include all four signed
zero combinations, infinities, each single-NaN placement, two NaNs with different
payload/sign bits, signaling NaNs, and Int64 values beyond Float64's exact range.
Use an independent integer/bit-classification oracle; apply shared resource,
validation, lifetime, invalidation and cancellation cases.

Current legacy maximum has Elementwise registration but rejects nonfinite values
through its shared reader. Its existing zero tie handling does not establish
this complete same-sign/mixed-sign contract. Versioned keys and these new dtype
and propagation rules remain unimplemented. No runtime tests are claimed here.
