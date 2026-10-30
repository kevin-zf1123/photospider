---
spec_schema_version: 1
id: FIL-05B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-05
function: percentile
proposed_operation_keys:
- filter.percentile_strict
- filter.percentile_accelerated_apple_silicon
- filter.percentile_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S04
---
# FIL-05B: percentile

Linear or discrete percentile.

Inherits [FIL-05](FIL-05_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.percentile_strict`, `filter.percentile_accelerated_apple_silicon`, `filter.percentile_accelerated_x86_64`.

Ports and support match FIL-05A: input field and binary UInt8 footprint produce an output of the input shape.

### Parameters

Require the FIL-05A footprint, axes, anchor, boundary, `cval`, and empty policy, plus `q∈[0,1]` and interpolation (`linear`, `lower`, `higher`, `nearest_even`, or `midpoint`). These are static parameters; constructors do not supply defaults.

## Mathematics

Order samples as in FIL-05A. Compute exact `h=(N−1)*q`, `i=floor(h)`, `j=ceil(h)`, `f=h−i`. Linear returns `RN_t((1−f)a_i+f*a_j)`; if f=0, copy the selected sample bits. `lower`/`higher` select i/j. `nearest_even` rounds h to an integer index using ties-to-even. `midpoint` copies when i=j and otherwise returns `RN_t((a_i+a_j)/2)`.

Footprint premises and boundary support follow FIL-05A. q is static control. Compute the integer indices from exact q; do not pre-round h to floating point. Do not substitute a different library quantile default.

## Acceptance

Check exact endpoints q=0 and q=1. On `[0,10]` at q=1/4, linear interpolation returns 2.5. At q=1/2, nearest-even selects index 0. Include large N and q values near index boundaries to detect double rounding.

## References

[S04](../research-sources.md#s04).
