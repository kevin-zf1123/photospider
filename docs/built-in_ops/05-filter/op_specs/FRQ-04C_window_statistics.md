---
spec_schema_version: 1
id: FRQ-04C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-04
function: window_statistics
proposed_operation_keys:
- frequency.window_statistics_strict
- frequency.window_statistics_accelerated_apple_silicon
- frequency.window_statistics_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S14
- S15
---

# FRQ-04C: window_statistics

Window statistics and equivalent noise bandwidth.

Inherits [FRQ-04 family contract](FRQ-04_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.window_statistics_strict`, `frequency.window_statistics_accelerated_apple_silicon`, `frequency.window_statistics_accelerated_x86_64`.

### Ports

`window`, a rank-2 Float64 tensor → `coherent_gain`, `mean_square`, and `enbw_bins`, each a Float64 tensor of shape `[1]`.

### Parameters

No additional `sample_rate` is needed. The window may contain any values supported by NUM and must contain at least one nonzero value. Require Σw ≠ 0 to define ENBW.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **E** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

Let N=HW. Compute `gain=RN64(Σw/N)`, `mean_square=RN64(Σw²/N)`, and `ENBW=RN64(N*Σw²/(Σw)²)`. Use exact sums from the same input window; do not derive ENBW from rounded gain.

## Behavior

Each output depends on the complete window. The three outputs may share exact sums but their definitions are not interchangeable.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For a rectangular all-ones window, outputs are (1,1,1). For [0,1,0], outputs are (1/3,1/3,3). ENBW requires a nonzero exact sum as a caller premise. ENBW is measured in 2D DFT bins and is not automatically converted to Hz.

## References

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)
