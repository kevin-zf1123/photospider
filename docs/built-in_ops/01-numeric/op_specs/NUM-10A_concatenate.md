---
spec_schema_version: 1
id: NUM-10A
parent_id: NUM-10
function: concatenate
proposed_operation_keys:
  - array.concatenate_strict
  - array.concatenate_accelerated_apple_silicon
  - array.concatenate_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-10A: concatenate

Concatenates an ordered sequence of 2..256 single-tensor Results along a required static Int64 `axis`. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

Inputs are 2..256 single-tensor Results; tensor schema/member names may vary. Complete sample shapes include batch axes, have rank 1..8, positive extents and at most 2^40 elements. Inputs share dtype (UInt8, Int64, Float32 or Float64), rank and all non-axis extents. The output axis length is the sum of input lengths, subject to the same output count limit. Output `values` uses `photospider.tensor`/`samples` with ordinary axes. The SDK helper takes the profile immediately after `axis`.

## Mathematics

For input lengths L[k] and prefixes P[k], output coordinate o selects the unique k with `P[k] <= o[axis] < P[k]+L[k]`; its source coordinate is o with axis replaced by `o[axis]-P[k]`. All element bits are copied, including signaling NaNs. There are no casts or broadcasting. All three CPU profiles are bitwise identical.

## Behavior

Each output sample depends on exactly one sample of one input. Inputs whose slab is not selected have no payload or producer dependency, although all static schemas are checked. Empty selection reads nothing.

## Acceptance

Compare against an independent ordered coordinate mapping: nonleading axes, repeated references, 65 and 256 ports, all four dtypes. A failing producer for an unselected slab does not affect the result.
