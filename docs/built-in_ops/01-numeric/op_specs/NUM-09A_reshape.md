---
spec_schema_version: 1
id: NUM-09A
parent_id: NUM-09
function: reshape
proposed_operation_keys:
  - array.reshape_strict
  - array.reshape_accelerated_apple_silicon
  - array.reshape_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-09A: reshape

Reshape preserves the row-major logical element sequence of a single-tensor Result. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

The source is a single-tensor Result with any schema/member key. Its complete `sample_shape()`, including batch axes, has rank 1..8, positive extents and at most 2^40 elements. Required static String `shape` is a canonical comma-separated list of positive extents with rank 1..8 and the same product. There is no inferred dimension or order parameter. The SDK helper takes `CpuNumericProfile` after `shape`.

Output port `values` uses `photospider.tensor` v1 with member `samples`; the target shape consists of ordinary axes. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are supported with unchanged dtype. Every raw element bit is preserved, including signaling NaNs, payloads and signed zeros; all three CPU profiles are bitwise identical. Invalid static shape or schema fails before execution.

## Mathematics

For source shape I, target shape O and global output coordinate o, let `k = sum_j o[j] * product_{m>j} O[m]`. Source coordinate j is `floor(k / product_{m>j} I[m]) mod I[j]`. This mapping is independent of physical strides.

## Behavior

Each output sample depends on exactly the one source sample given by the mapping. Final coverage is the selected output region in global coordinates. Empty selection reads nothing.

## Acceptance

Source `[2,3]` with logical values `[0,1,2,3,4,5]`, target `[3,2]` gives `[[0,1],[2,3],[4,5]]`. Compare against independent integer flatten/unflatten and raw-bit comparison: partial tiles, changed rank, all raw dtypes.
