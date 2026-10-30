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

This specification defines the intended mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member has been marked Accepted or as a production implementation.

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-08A | [Scalar-NSR Wiener restoration](RES-08A_wiener_nsr.md) | primitive | F |
| RES-08B | [Periodic Tikhonov restoration](RES-08B_tikhonov_periodic.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

### Native periodic operator

Observation y is a signed numerical field. Convert PSF taps to the input dtype, normalize by a native positive sum, fold periodically at the explicit anchor, and form the native FFT response H. The mathematical operator A is the finite periodic convolution with those coefficients. Evaluate the DFT, complex products, denominator and inverse DFT in native input precision. Preserve FRQ transform normalization and boundary conjugacy. Report conditioning-sensitive error; near-singular amplification is not a reason to claim exact recovery.

If the computed native frequency denominator is zero, apply explicit `singular=zero|error`. Zero sets the estimate to complex +0, a minimum-energy fill for the missing frequency. Error reports the defined valid-input no-solution outcome. Nonnegative lambda/nsr and positive PSF mass are unchecked caller premises. Do not clip output or apply gamma/tone mapping. A nonperiodic inverse problem requires a separately specified matched A/AT model.

## Shared Execution and Numeric Obligations

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; each member defines mathematical support and fixed compiled reads.

### Native evaluation and ownership

Native FFT execution follows class F. No exact-restoration-expression certification or NUM fallback is required. Spectra, normalized PSF, solver tables and workspace must be reserved and charged to their documented owners. Exhausted capacity/work or cancellation fails the invocation. All accepted internal work drains before completion. Retained immutable Results or PreparedPlans may outlive Context without extending borrowed invocation storage.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S09 · FFTW: DFT definition](../research-sources.md#s09)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
