---
spec_schema_version: 1
id: FIL-16B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-16
function: detail_gain
proposed_operation_keys:
- filter.detail_gain_strict
- filter.detail_gain_accelerated_apple_silicon
- filter.detail_gain_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---

# FIL-16B: detail_gain

Explicit base/detail gain.

Inherits [FIL-16](FIL-16_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.detail_gain_strict`, `filter.detail_gain_accelerated_apple_silicon`, `filter.detail_gain_accelerated_x86_64`.

`base,detail` same shape/dtype → `output`.

### Parameters

gain is explicitly supplied and finite; there is no hidden threshold.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

At gain=0, bit-copy base (detail does not participate); otherwise evaluate the exact fused expression `RN_t(base+gain*detail)`. This is not the similarly named input form I+a*(I-B).

## Behavior

Each output position depends on base; when gain≠0 it also depends on detail. gain is a static parameter.

## Acceptance

base=2, detail=3, gain=2 yields 8; zero gain bypasses poisoned detail; large opposite terms detect unintended intermediate overflow.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)
