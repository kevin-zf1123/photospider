---
spec_schema_version: 1
id: FIL-13A
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: FIL-13
function: canny_quantized4
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
research_sources:
- S06
---

# FIL-13A: canny_quantized4

Static Canny workflow.

Inherits [FIL-13](FIL-13_contract.md), the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

`input` is a single-plane floating-point field → `edges` UInt8 with the same shape.

The public SDK helper is `ps::filter::canny_quantized4` (declared in `photospider/ops/filter.hpp`). It appends explicit Gaussian, unit-ramp Sobel, magnitude, quantized-four-direction NMS and hysteresis stages and returns the final `edges` reference. There is no opaque Canny arithmetic key. `CannyOptions` explicitly supplies axes, sigmas, radii, norm, thresholds and connectivity.

### Parameters

Gaussian sigma and radius are explicit; the gradient is fixed to FIL-08B unit_ramp with dx=dy=1; magnitude=l1|l2; 0<low<high; explicit connectivity=4|8; axes are explicit; every spatial stage uses boundary=reflect_half.

## Mathematics

Numerical class: **S**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

First apply FIL-04B, FIL-08B, and FIL-09A, preserving each member’s RN_t stage. NMS compares the original magnitude with neighbors in two directions: choose x when abs(gx)≥2abs(gy), choose y when abs(gy)≥2abs(gx), otherwise choose (1,1) or (1,-1) according to sign(gx*gy). Ties prefer the first, x-axis branch; a zero vector produces zero response. An out-of-bounds neighbor is treated as +Inf, suppressing the edge. Keep the center only when it is ≥ the positive-direction neighbor and > the negative-direction neighbor; otherwise set it to zero. Mark NMS values ≥low as weak and ≥high as strong, then apply B connectivity.

## Behavior

The edge output is complete-input for each plane. Gaussian and gradient stages remain local, but hysteresis reads the complete NMS plane. Batches do not read one another.

## Acceptance

A constant input produces all zeros; a long weak chain connected to a strong pixel across multiple tiles is retained; plateau ties are deterministic on the same platform; production inputs satisfy 0<low<high, while an ROI result matches the corresponding crop of the complete-input result.

The RN_t stage boundaries of the composed members are preserved.

## References

[S06 · scikit-image feature](../research-sources.md#s06)
