---
spec_schema_version: 1
id: FRQ-03B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-03
function: ifftshift
proposed_operation_keys:
- frequency.ifftshift_strict
numeric_reference: bitwise copy
oracle_scope: mathematical_reference
research_sources:
- S09
- S10
---

# FRQ-03B: ifftshift

Inverse centered frequency shift.

Inherits [FRQ-03 family contract](FRQ-03_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation key: `frequency.ifftshift_strict`.

### Ports

`real, imag` full grid → an output pair with identical shape; each component may be requested independently.

### Parameters

Required parameter: axis. Input `index_order` must be `shifted`. Reject half grids; explicitly reconstruct the full grid first.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **bitwise copy** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

For each selected axis, compute `out[q]=input[(q+floor(N/2)) mod N]` and copy bits unchanged, including NaN, Inf, and −0; no finite-value validation is performed. Set `index_order=unshifted`.

## Behavior

The mathematical support is a one-to-one permutation: each output sample reads exactly one input sample of its own component.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

For length 5, `fftshift([0,1,2,3,4])` gives `[3,4,0,1,2]` and `ifftshift` restores the input. For length 4, shift by two positions. Applying `fftshift` twice to an odd-length axis is not the identity.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10)
