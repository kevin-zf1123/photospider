---
spec_schema_version: 1
id: RES-12A
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-12
function: paired_notch_denoise
proposed_operation_keys: []
numeric_reference: F
oracle_scope: composed_math_references
---

# RES-12A: paired_notch_denoise

Explicit paired-notch suppression of periodic interference.

Inherits the [RES-12 family contract](RES-12_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). Native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Interface

### Ports

`input` → `output` with the same shape.

### Parameters

All grid and notch parameters from FRQ05C; FRQ01 norm=backward; input is treated as periodic. No window is applied implicitly (the caller may add one explicitly; a window with zeros cannot be silently inverted).

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

FRQ02A → explicitly generate the full FRQ05C response and select the corresponding half → FRQ06A → FRQ02B. Each node publishes its declared output stage; this helper is class F and preserves the called node interfaces. The response must preserve boundary conjugacy during construction; do not repair violations by discarding the imaginary component.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic and reduction regrouping follow the [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Behavior

Each output depends on the complete input plane; centers are static parameters. Reflection padding/cropping requires a separate helper and must not silently change the boundary.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

A depth-zero chain does not guarantee a general bitwise round trip. A sinusoid over an integer number of periods demonstrates notch attenuation; loss of real texture at the same frequency is a quality-negative case.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)
