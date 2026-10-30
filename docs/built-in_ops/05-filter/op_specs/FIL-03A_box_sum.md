---
spec_schema_version: 1
id: FIL-03A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-03
function: box_sum
proposed_operation_keys:
- filter.box_sum_strict
- filter.box_sum_accelerated_apple_silicon
- filter.box_sum_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
---
# FIL-03A: box_sum

Rectangular sum.

Inherits [FIL-03](FIL-03_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.box_sum_strict`, `filter.box_sum_accelerated_apple_silicon`, `filter.box_sum_accelerated_x86_64`.

Input `input` is a raw floating-point field; output `output` has the same shape.

### Parameters

Require axes, positive integer `height` and `width`, an explicit anchor, boundary, and `cval`. Shape is fixed to `same`. Truncation is not supported for sum. No constructor defaults.

## Mathematics

Every footprint tap has unit weight. A tap `(j,i)` samples `(y+j-anchor_y,x+i-anchor_x)` using the correlation coordinate convention, including even and off-center footprints. Return `RN_t(ΣI)` with no normalization. Truncation is not supported for sum.

The mapped support is the complete rectangular footprint; repeated extension preserves mathematical multiplicity, while truncation reads no out-of-domain samples.

## Acceptance

For a constant field of 2 and a 3x3 footprint, sum is 18 and mean is 2. Also cover a single-pixel image, a kernel larger than the image, truncated corner sample counts, and even-sized off-center footprints.

## References

[S01](../research-sources.md#s01).
