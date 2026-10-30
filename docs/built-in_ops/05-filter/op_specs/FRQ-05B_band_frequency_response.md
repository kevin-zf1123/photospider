---
spec_schema_version: 1
id: FRQ-05B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-05
function: band_frequency_response
proposed_operation_keys:
- frequency.band_frequency_response_strict
- frequency.band_frequency_response_accelerated_apple_silicon
- frequency.band_frequency_response_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S09
- S26
---

# FRQ-05B: band_frequency_response

Radial band-pass or band-stop response.

Inherits [FRQ-05 family contract](FRQ-05_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.band_frequency_response_strict`, `frequency.band_frequency_response_accelerated_apple_silicon`, `frequency.band_frequency_response_accelerated_x86_64`.

### Ports

The output shape, dtype, grid descriptor, and general field semantics are the same as FRQ-05A.

### Parameters

Use the response kind defined by FRQ-05A. Require `0 < cutoff_low < cutoff_high`; `pass` is `band` or `reject`. `order` is required only for Butterworth.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **B** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

Use the exact `L(r;fc)` response defined by FRQ-05A. The smooth band response is `L(r;cutoff_high)-L(r;cutoff_low)` and the reject response is `1-band`. For the ideal profile, the band response is 1 when `cutoff_low < r ≤ cutoff_high`, and 0 otherwise. Round the complete expression once to RN64. Do not rescale the peak to force a maximum of 1.

## Behavior

Evaluate pointwise from the static grid.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

At DC, the band-pass response is 0 and the band-stop response is 1. The ideal response excludes the low cutoff and includes the high cutoff. The maximum of a smooth band response is generally below 1.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
