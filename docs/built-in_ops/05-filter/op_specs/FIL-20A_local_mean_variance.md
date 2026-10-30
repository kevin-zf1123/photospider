---
spec_schema_version: 1
id: FIL-20A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-20
function: local_mean_variance
proposed_operation_keys:
- filter.local_mean_variance_strict
- filter.local_mean_variance_accelerated_apple_silicon
- filter.local_mean_variance_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S05
---

# FIL-20A: local_mean_variance

Local mean and unbiased/population variance.

Inherits [FIL-20](FIL-20_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.local_mean_variance_strict`, `filter.local_mean_variance_accelerated_apple_silicon`, `filter.local_mean_variance_accelerated_x86_64`.

`input` → `mean,variance` with the same shape and dtype; outputs are independent.

### Parameters

Box height/width/anchor are explicit; boundary is a shared mode such as reflect_half or truncate; cval=0; ddof is an integer ≥0.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

For N actual taps, require N>ddof. mean=`RN_t(S1/N)`; variance=`RN_t((N*S2-S1²)/(N*(N-ddof)))`, with exact S1=Σx and S2=Σx². The rounded mean is not used to compute variance.

## Behavior

Each output depends on the complete box support. N is determined from the static footprint and boundary configuration. N>ddof is a numerical denominator premise for the variance calculation.

## Acceptance

For [1,2,3], population variance is 2/3 and unbiased variance is 1. Test small differences under a large common offset; constant input yields +0; a truncated corner with N≤ddof is outside the variance premise.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S05 · He, Sun, Tang: Guided Image Filtering](../research-sources.md#s05)
