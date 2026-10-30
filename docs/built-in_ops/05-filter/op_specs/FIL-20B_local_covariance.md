---
spec_schema_version: 1
id: FIL-20B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-20
function: local_covariance
proposed_operation_keys:
- filter.local_covariance_strict
- filter.local_covariance_accelerated_apple_silicon
- filter.local_covariance_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S05
---

# FIL-20B: local_covariance

Two-field local covariance.

Inherits [FIL-20](FIL-20_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation keys: `filter.local_covariance_strict`, `filter.local_covariance_accelerated_apple_silicon`, `filter.local_covariance_accelerated_x86_64`.

`x,y` same shape/dtype → `covariance`.

### Parameters

Same as FIL-20A.

## Mathematics

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

`RN_t((N*Σxy-Σx*Σy)/(N*(N-ddof)))`; the result may be negative. Do not first compute two rounded means; covariance does not use the incorrect centered approximation used by some variance implementations.

## Behavior

Complete windows of x and y, paired plane by plane. There is no implicit all-channel covariance; multichannel matrices require explicit pairing.

## Acceptance

When y=x, match FIL-20A variance; y=-x gives negative covariance; a constant side gives zero; exact cancellation yields +0.

## References

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S05 · He, Sun, Tang: Guided Image Filtering](../research-sources.md#s05)
