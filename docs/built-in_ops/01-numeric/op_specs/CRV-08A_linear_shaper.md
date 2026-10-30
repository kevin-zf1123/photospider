---
spec_schema_version: 1
id: CRV-08A
parent_id: CRV-08
function: linear_shaper
proposed_template_names:
  - curve.linear_shaper
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-08A: linear_shaper

The public `linear_shaper` helper expands ordinary workflow nodes; it does not register a new primitive.

## Interface

Ordered inputs are single-tensor Results input, lower and upper, with arbitrary schema/member names. Complete sample shapes include batch axes. Input is Float32/Float64, rank 1..8, positive extents and at most 2^40 elements. Bounds are same-dtype scalars with shape `[1]`. Output `values` uses `photospider.tensor`/`samples` with ordinary input axes and unchanged dtype. Shape/dtype mismatches fail before execution.

## Mathematics

The formula is `(input-lower)/(upper-lower)`, with one final rounding of the complete remap expression and no clipping. Finite ordered bounds `lower<upper` are an unchecked caller precondition. Within that domain, endpoint bits, NaN quieting/payload, signed zero and infinity extensions follow [remap_range](NUM-06B_remap_range.md). Strict results are bitwise reproducible; accelerated profiles retain the [shared floating bound](NUM_accelerated_contract.md). Invalid bound payloads have no promised runtime diagnostic.

## Conceptual DAG

The helper constructs scalar zero/one literals (count-one linspace with direct rounding for Float32), expands bounds and literals through constant operations, and applies remap_range; see the [CRV-08 linear composition](CRV-08_shaper.md#conceptual-dag-linear-composition).

## Acceptance

For a valid fixture, lower=-2, upper=2, input=[-2,0,2,4] gives [0,0.5,1,1.5]. Expected values come from an exact rational formula, including endpoints and IEEE specials. Out-of-domain bounds are not rejection tests.
