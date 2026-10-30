---
spec_schema_version: 1
id: NUM-07H
parent_id: NUM-07
function: select
proposed_operation_keys:
  - numeric.select_strict
  - numeric.select_accelerated_apple_silicon
  - numeric.select_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07H: select

Numeric profile: strict is the exact reference. Results are raw-bit copies and bitwise identical across the three CPU profiles. Inherits the [NUM baseline](NUM_common_contract.md) and the [comparison contract](NUM-07_comparison_contract.md); explicit rules below take precedence.

## Interface

Selects an element from one of two arrays using a boolean-valued UInt8 condition. Inputs in order are `condition`, `when_true`, `when_false`, all with the same rank-1..8 positive shape (including batch axes). Both value branches share one dtype from UInt8, Int64, Float32 and Float64; the condition is UInt8. Output `values` uses the branch dtype, unchanged shape and no attached domain description. There are no static parameters or implicit broadcasts/casts. Keys: `numeric.select_strict`, `numeric.select_accelerated_apple_silicon`, `numeric.select_accelerated_x86_64`.

## Mathematics

The caller supplies condition bytes exactly 0 or 1 at every computed coordinate (unchecked precondition). Zero selects `when_false`; one selects `when_true`. The selected element's exact bits are copied, including NaN payload, signaling NaN, infinity and signed zero, without arithmetic quieting.

## Behavior

Each output sample depends on all three inputs at the same coordinate. Both branch owners remain dependencies even when every condition selects the same branch, so an upstream failure in a declared branch can prevent execution. Empty selection reads nothing.

## Acceptance

Fixture: condition=[1,0,1], when_true=[10,20,30], when_false=[1,2,3] gives [10,2,30]. Cases: all-true, all-false and alternating conditions, all four branch dtypes, distinct signaling-NaN payloads and signed zero.
