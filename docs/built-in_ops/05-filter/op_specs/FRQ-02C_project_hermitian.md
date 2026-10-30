---
spec_schema_version: 1
id: FRQ-02C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-02
function: project_hermitian
proposed_operation_keys:
- frequency.project_hermitian_strict
- frequency.project_hermitian_accelerated_apple_silicon
- frequency.project_hermitian_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S10
---

# FRQ-02C: project_hermitian

Explicit Hermitian-symmetry projection.

Inherits [FRQ-02 family contract](FRQ-02_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.project_hermitian_strict`, `frequency.project_hermitian_accelerated_apple_silicon`, `frequency.project_hermitian_accelerated_x86_64`.

### Ports

`real,imag` full, unshifted grid → `out_real, out_imag` with the same grid descriptor.

### Parameters

Required parameters are axes and a compatible grid descriptor. Do not infer missing pairs directly from half-spectrum data.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **E** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

For each pair k and −k, compute `a=RN_t((ReF[k]+ReF[−k])/2)` and `b=RN_t((ImF[k]−ImF[−k])/2)`. Use the lower row-major index as representative and set the other output to `(a,−b)`; zero imaginary values become +0. At self-conjugate locations, output imaginary +0 and copy the real component bitwise. Arithmetic follows NUM; this projection does not validate or claim that the input pair was conjugate. An output containing NaN may fall outside the downstream `irfft` conjugacy premise.

## Behavior

Each requested coefficient q depends on its conjugate partner. Static grid shape, axes, packing and index-order configuration are checked statically; numerical and payload association premises remain unchecked.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Acceptance must verify bitwise idempotence on the specified NUM domain, +0 imaginary output at self-conjugate locations, and correctly rounded pair averages without avoidable intermediate overflow. A projected output containing NaN may fall outside the downstream irfft conjugacy premise. Projection does not certify that premise or trigger a consumer validation pass.

## References

[S10 · FFTW: real-data array format](../research-sources.md#s10)
