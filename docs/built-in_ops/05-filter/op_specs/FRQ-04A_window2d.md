---
spec_schema_version: 1
id: FRQ-04A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-04
function: window2d
proposed_operation_keys:
- frequency.window2d_strict
- frequency.window2d_accelerated_apple_silicon
- frequency.window2d_accelerated_x86_64
numeric_reference: B+S
oracle_scope: mathematical_reference
research_sources:
- S14
- S15
---

# FRQ-04A: window2d

Two-dimensional Hann, Tukey, or rectangular window.

Inherits [FRQ-04 family contract](FRQ-04_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.window2d_strict`, `frequency.window2d_accelerated_apple_silicon`, `frequency.window2d_accelerated_x86_64`.

### Ports

No dynamic input → `window` Float64 `[height,width]`.

### Parameters

`height` and `width` must be at least 1 ; Required `kind`: `rectangular`, `hann`, or `tukey` ; sampling=symmetric|periodic (required); Tukey alpha∈[0,1] (required when kind=tukey); For other window kinds, `alpha` must be omitted.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **B+S** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

The rectangular window is 1. The Hann window is `w(n)=.5*(1−cos(2πn/D))`. For Tukey, let t=n/D; alpha=0 gives rectangular and alpha=1 gives Hann. For `0≤t<alpha/2`, use `.5*(1+cos(π*(2t/alpha−1)))`; for `alpha/2≤t≤1−alpha/2`, use 1; otherwise use `.5*(1+cos(π*(2t/alpha−2/alpha+1)))`. Round the complete 1D expression to RN64, then form the 2D outer product. Endpoints and the middle plateau use exact rational special cases.

## Behavior

Static parameters only. Each output coordinate reads its corresponding derived wx and wy.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Acceptance fixtures: symmetric Hann N=3 gives [0,1,0]; periodic Hann N=4 gives [0,.5,1,.5]; N=1 gives [1]. Tukey alpha=0 and alpha=1 must match the corresponding rectangular and Hann endpoints.

## References

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)
