---
spec_schema_version: 1
id: NUM-09C
parent_id: NUM-09
function: slice
proposed_operation_keys:
  - array.slice_strict
  - array.slice_accelerated_apple_silicon
  - array.slice_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-09C: slice

Slice preserves rank and selects `s[j]=starts[j]+o[j]*steps[j]` from one source Result. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

The source has one tensor under any schema/member key; its complete `sample_shape()` includes batch axes. Dynamic `starts` and `steps` are Int64 `[rank]` Result inputs. Required static String `counts` is a canonical comma-separated list of positive output extents. Rank is 1..8 and both logical counts are at most 2^40. Each output extent does not exceed its source extent. The SDK helper takes the profile after `counts`.

Output port `values` uses `photospider.tensor` v1/member `samples`, with ordinary axes and unchanged dtype. UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64 are copied bit-for-bit, including signaling NaNs. There is no implicit cast, rank squeezing, negative-index reinterpretation or empty slice. All three CPU profiles preserve identical bits.

## Dynamic domain

Starts are nonnegative absolute indices below each source extent. For counts greater than one, steps are nonzero and `start+(count-1)*step` remains in bounds for the complete counts, including unselected coordinates. These are unchecked caller premises; selected addresses are computed with widened integer arithmetic.

A singleton axis ignores its step numerically. If every count is one, the step payload and its producer are not read (its static schema is still checked). Otherwise the complete steps vector is read, including entries belonging to singleton axes. Every nonempty selection reads the complete starts vector. Empty selection reads no controls or source payload.

## Behavior

The source dependency covers all coordinates reachable by legal controls. For input extent I, count N and selected output interval `[u,u+e)`, a singleton axis requires `[0,I)`; otherwise the possible source set is `[u,I-N+u+e) union [N-u-e,I-u)`, with positive-step possibilities contained in step +1 and negative-step possibilities in step -1. This set can exceed the source accessed by one Run but excludes coordinates unreachable by any legal control binding.

## Acceptance

Input `[0,1,2,3,4,5]`, starts `[4]`, steps `[-2]`, counts `"3"` gives `[4,2,0]`. Cases: valid complete-domain controls with partial output, all-singleton step elision, dynamic control changes, extreme ignored steps, all seven raw dtypes, against independent signed integer coordinate enumeration.
