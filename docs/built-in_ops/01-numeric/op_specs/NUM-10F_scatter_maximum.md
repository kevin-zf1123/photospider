---
spec_schema_version: 1
id: NUM-10F
parent_id: NUM-10
function: scatter_maximum
proposed_operation_keys:
  - array.scatter_maximum_strict
  - array.scatter_maximum_accelerated_apple_silicon
  - array.scatter_maximum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-10F: scatter_maximum

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

Select the numerical maximum including base. For floating NaNs and zero signs,
use the shared aggregate priority and NUM-05 maximum rules; this operation never
ignores a NaN or converts Int64 through floating point.

If no update matches, copy base without arithmetic, including signaling-NaN
bits. For aggregate variants with matching updates, use the shared exceptional
value priority and zero rules. Only matching contributors enter arithmetic, but
an upstream or typed-validation failure from any active input can fail Whole
preparation.

## Acceptance and implementation status

Conceptual public fixture: base=[10,20,30], indices=[1,1,2], updates=[2,3,4],
axis=0 -> [10,20,30]. With the same base/indices and updates=[22,23,34],
expect [10,23,34]; this case also detects an implementation that only copies base.
Use independent index grouping and the specified exact
numeric or bit-selection oracle; verify each port's actual read footprint.
Include duplicated targets, unhit sNaN base, selected and unselected failures,
NaN payload precedence, infinities, signed zeros, integer extrema, source strides,
index changes, global invalid-index rejection and shared resource/lifetime tests.

The public workflow checks all four scatter variants and the aggregate's
first-NaN quieting behavior on its focused Float64 fixture. The independent
`index_oracle.py` checks maximum's NaN and signed-zero ordering across generated
cases for strict and Apple profiles. See
[NUM-10 Whole execution](../indexing-whole.md) for runnable commands and
evidence boundaries.
