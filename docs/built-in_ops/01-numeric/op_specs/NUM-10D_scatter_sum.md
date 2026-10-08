---
spec_schema_version: 1
id: NUM-10D
parent_id: NUM-10
function: scatter_sum
proposed_operation_keys:
  - array.scatter_sum_strict
  - array.scatter_sum_accelerated_apple_silicon
  - array.scatter_sum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-10D: scatter_sum

The strict key follows its exact numeric reference. Accelerated floating
results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md)
where arithmetic applies; raw copies and discrete results remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements. The rules below
specify the NUM-10 behavior.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inherit the [scatter contract](NUM-10_scatter_contract.md) for ports, shapes,
Whole demand, index validation, storage and shared exceptional-value rules.
Every input is a Result with exactly one tensor member at an unrestricted key;
its `sample_shape()` includes batch axes. The `values` output is a Result using
`photospider.tensor` / `samples`, with full ordinary output shape and no facets
or batch topology. Nonempty demand reads complete `base`, `indices` and `updates`
with data, validation and descriptor support (role 13); Empty demand reads no
payload and performs no sample arithmetic. The kernel computes the complete
output before consumer projection. The base is immutable.

## Update semantics

Sum base and all matching updates as exact mathematical numbers, with one final
round-to-nearest/ties-even conversion to Float32/Float64 or final UInt8/Int64
range check. Do not round or reject intermediate partial sums. Integer overflow
is based only on the final result; floating overflow returns signed infinity.
Use an exact accumulator or certified equivalent, with actual limb/work capacity
charged to host budgets. No unordered floating atomic accumulation is allowed.

If no update matches, copy base without arithmetic, including signaling-NaN
bits. For aggregate variants with matching updates, use the shared exceptional
value priority and zero rules. Only matching contributors enter arithmetic, but
an upstream or typed-validation failure from any active input can fail Whole
preparation.

## Acceptance and implementation status

Conceptual public fixture: base=[10,20,30], indices=[1,1,2], updates=[2,3,4],
axis=0 -> [10,25,34]. Use independent index grouping and the specified exact
numeric or bit-selection oracle; verify each port's actual read footprint.
Include duplicated targets, unhit sNaN base, selected and unselected failures,
NaN payload precedence, infinities, signed zeros, integer extrema, source strides,
index changes, global invalid-index rejection and shared resource/lifetime tests.

For Int64, base=INT64_MAX with matching updates [1,-1] must return INT64_MAX
without a partial-sum overflow failure. The same grouping principle applies to
finite floating cancellation; compare exact sums with one final rounding.

The public workflow checks all four scatter variants, exact cancellation and
final integer overflow. The existing `test_numeric_result_math` integration
fixture contains additional scatter cases; it was not rerun for this Result
migration and does not establish a complete dtype/backend matrix. See
[NUM-10 Whole execution](../indexing-whole.md) for runnable commands and
evidence boundaries.
