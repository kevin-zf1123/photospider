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

This specification defines the intended mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member has been marked Accepted or as a production implementation.

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-08A | [Scalar-NSR Wiener restoration](RES-08A_wiener_nsr.md) | primitive | E |
| RES-08B | [Periodic Tikhonov restoration](RES-08B_tikhonov_periodic.md) | primitive | E |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. A primitive may propose strict and accelerated profiles; a helper only orchestrates explicit stages.

## Shared Mathematics, Units, and Profiles


### Exact Operator
Observation y is a signed numerical field; nonfinite arithmetic follows NUM. The Float64 PSF is nonnegative with exact sum>0; it may exceed the image size and is folded periodically. Specify the anchor. Normalize the PSF by its exact sum. A is the resulting finite periodic convolution matrix, and H its exact DFT frequency response (do not first RN it into a spectral tensor). The DFT/IFFT, complex products, and denominator in the restoration formula form one E mathematical expression, with one final RN_t per pixel. An explicitly composed rounded S chain of FRQ nodes is not this member’s strict profile and may be used as a separate experimental workflow.
If a frequency denominator is exactly zero, apply the explicitly supplied `singular=zero|error` policy. The zero policy sets that frequency estimate to complex +0, an explicit minimum-energy fill for missing frequencies; it does not claim to recover information. Reject negative lambda/nsr. Self-conjugate frequencies ensure a real result. Do not clip the output or apply automatic gamma/tone mapping. The initial profile does not accept reflected PSF boundaries; a nonperiodic inverse problem requires a separate key and matched A/AT definitions.


## Shared Execution and Numeric Obligations

E denotes evaluation of the exact expression followed by one final RN_t; B denotes explicitly generated, fixed coefficients; S denotes explicitly staged rounding. See the [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Members must still validate their actual parameter, control-model, and solution-domain constraints. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable; validating payloads of an unrequested component must not introduce a hidden whole-image dependency.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S09 · FFTW: DFT definition](../research-sources.md#s09)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
