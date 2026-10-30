---
spec_schema_version: 1
id: NUM-09B
parent_id: NUM-09
function: transpose
proposed_operation_keys:
  - array.transpose_strict
  - array.transpose_accelerated_apple_silicon
  - array.transpose_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-09B: transpose

Transpose permutes the logical axes of a single-tensor Result. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

The source is a single-tensor Result with any schema/member key. Its complete `sample_shape()` includes batch axes, has rank 1..8, positive extents and at most 2^40 elements. Required static String `permutation` is a canonical comma-separated list containing each input axis exactly once; the SDK helper takes the profile immediately after `permutation`. Invalid permutations, schemas and static metadata fail before execution.

Output port `values` uses `photospider.tensor` v1/member `samples`, with ordinary axes and unchanged dtype. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are supported. Raw bits, including signaling NaNs and signed zeros, are preserved identically by all three CPU profiles. No image/color roles or arithmetic conversions are inferred.

## Mathematics

For permutation p and input shape I, the output shape is `O[j]=I[p[j]]`, and each global output coordinate o reads source `s[p[j]]=o[j]`.

## Behavior

Each output sample depends on exactly one source sample. Empty selection reads nothing.

## Acceptance

Source `[2,3,4]` with values `100*i+10*j+k` and permutation `[2,0,1]` gives output `[4,2,3]` with `values[k,i,j]=100*i+10*j+k`. Cases: identity, rank-2/rank-3 permutations, rank eight, all raw dtypes.
