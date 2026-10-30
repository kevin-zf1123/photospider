---
spec_schema_version: 1
id: RES-08
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-08A
- RES-08B
---

# RES-08: Linear Frequency-Domain Restoration

Wiener NSR and Tikhonov use different parameters; the initial contract defines periodic boundaries only.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-08A | [Scalar-NSR Wiener restoration](RES-08A_wiener_nsr.md) | primitive | F |
| RES-08B | [Periodic Tikhonov restoration](RES-08B_tikhonov_periodic.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

## Mathematics

### Native periodic operator

Observation y is a signed numerical field. Convert PSF taps to the input dtype, normalize by a native positive sum, fold periodically at the explicit anchor, and form the native FFT response H. The mathematical operator A is the finite periodic convolution with those coefficients. Evaluate the DFT, complex products, denominator and inverse DFT in native input precision. Preserve FRQ transform normalization and boundary conjugacy. Report conditioning-sensitive error; near-singular amplification is not a reason to claim exact recovery.

If the computed native frequency denominator is zero, apply explicit `singular=zero|error`. Zero sets the estimate to complex +0, a minimum-energy fill for the missing frequency. Error reports the defined valid-input no-solution outcome. Nonnegative lambda/nsr and positive PSF mass are unchecked caller premises. Do not clip output or apply gamma/tone mapping. A nonperiodic inverse problem requires a separately specified matched A/AT model.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

Native FFT execution follows class F. No exact-restoration-expression certification or NUM fallback is required.

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S22 · scikit-image restoration](../research-sources.md#s22);[S09 · FFTW: DFT definition](../research-sources.md#s09)
