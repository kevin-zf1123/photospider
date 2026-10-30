---
spec_schema_version: 1
id: FRQ-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-04
function: apply_window
proposed_operation_keys:
- frequency.apply_window_strict
- frequency.apply_window_accelerated_apple_silicon
- frequency.apply_window_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S14
- S15
---

# FRQ-04B: apply_window

Apply a window explicitly.

Inherits [FRQ-04 family contract](FRQ-04_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.apply_window_strict`, `frequency.apply_window_accelerated_apple_silicon`, `frequency.apply_window_accelerated_x86_64`.

### Ports

`input, window` → `output`. The window is Float64 and has the same spatial shape. Non-spatial axes may be broadcast only through an explicit `broadcast_axes` list.

### Parameters

Required parameters: axis and the `broadcast_axes` list. Do not divide automatically by coherent gain or energy.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **E** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

At each point, compute `RN_t(input*window)`.A zero window coefficient does not suppress normal NUM arithmetic: the corresponding multiplication still reads and processes the input, including NaN/Inf propagation.

## Behavior

Pointwise arithmetic consumes the input and corresponding window coordinates. Preserve the declared non-spatial member layout; downstream interpretation is supplied explicitly by the caller.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A +0 Hann endpoint multiplied by a positive finite input produces +0; multiplied by a negative finite input it produces −0. Zero operands retain the NUM multiplication sign rules. NaN inputs follow NUM even where the corresponding window coefficient is zero. No brightness compensation is applied.

## References

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)
