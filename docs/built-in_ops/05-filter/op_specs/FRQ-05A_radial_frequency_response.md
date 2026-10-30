---
spec_schema_version: 1
id: FRQ-05A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-05
function: radial_frequency_response
proposed_operation_keys:
- frequency.radial_frequency_response_strict
- frequency.radial_frequency_response_accelerated_apple_silicon
- frequency.radial_frequency_response_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S09
- S26
---

# FRQ-05A: radial_frequency_response

Radial low-pass or high-pass response.

Inherits [FRQ-05 family contract](FRQ-05_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.radial_frequency_response_strict`, `frequency.radial_frequency_response_accelerated_apple_silicon`, `frequency.radial_frequency_response_accelerated_x86_64`.

### Ports

No dynamic input → `real,imag` full response.

### Parameters

Required grid parameters; `kind` (`ideal`, `gaussian`, or `butterworth`); `pass` (`low` or `high`); and `cutoff > 0`. An Int64 `order ≥ 1` is required for Butterworth only.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **B** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

For low-pass, the ideal response is 1 when r≤fc and 0 otherwise, including the cutoff. The Gaussian response is `exp(−r²/(2fc²))`, where fc is sigma, not the half-power radius. The Butterworth response is `1/sqrt(1+(r/fc)^(2n))`, with fc defined at amplitude −3 dB. For high-pass, compute the exact real expression `1−L(r)` first and round the complete expression once to RN64; do not subtract the rounded low-pass value. The imaginary component is +0.

## Behavior

There is no source-data demand. Each q is determined by the grid and static parameters. Positive and negative frequencies use the same baked value.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

At DC, low-pass is 1 and high-pass is 0. At the cutoff, Butterworth is RN64(1/sqrt2), and Gaussian is RN64(exp(−.5)). These three cutoff parameters do not represent the same visual radius.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
