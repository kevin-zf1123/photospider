---
spec_schema_version: 1
id: FRQ-05C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: implemented
parent_id: FRQ-05
function: paired_notch_response
proposed_operation_keys:
- frequency.paired_notch_response_strict
- frequency.paired_notch_response_accelerated_apple_silicon
- frequency.paired_notch_response_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S09
- S26
---

# FRQ-05C: paired_notch_response

Paired periodic-frequency notch response.

Inherits [FRQ-05 family contract](FRQ-05_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Interface

Operation keys: `frequency.paired_notch_response_strict`, `frequency.paired_notch_response_accelerated_apple_silicon`, `frequency.paired_notch_response_accelerated_x86_64`.

### Ports

No dynamic input → real/imag response.

### Parameters

Require a nonempty static center list `(fy,fx)` in cycles per unit within the Nyquist fundamental domain; `sigma_y/x > 0`; and `depth ∈ [0,1]`. Callers guarantee distinct centers and conjugacy orbits, including periodic equivalents and explicitly supplied opposite centers; these numerical premises introduce no production validation pass. Canonicalize the caller-supplied order for the descriptor only; the exact product is order-independent.

NaN, infinities and floating-point overflow follow the corresponding NUM operation; no finite-only input rule applies and overflow is not an operation failure. All applicable parameters are supplied explicitly, without constructor defaults; parameters that do not apply to the selected profile are omitted. Real parameters denote their stored Float64 values.

## Mathematics

Numeric reference: **B** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

For each center c, include both +c and −c, counting a self-conjugate center once. Define torus difference δf as the nearest representative modulo frequency period 1/d; choose the positive representative on a half-period tie. Compute `N_c=1−depth*exp(−.5*((δfy/sy)²+(δfx/sx)²))`; H is the exact product of all Nc, with the complete expression rounded once to RN64. Periodic distance makes the response continuous around Nyquist; do not truncate Euclidean distance to the rectangular array.

## Behavior

No image-data input; every output sample depends on the complete center list. Conjugate frequency pairs share identical response bits.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

With depth=0, the response is all 1. A center at zero contributes one notch rather than a squared notch. The responses for paired ±c centers match. Frequencies on opposite sides of the Nyquist edge are equivalent.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
