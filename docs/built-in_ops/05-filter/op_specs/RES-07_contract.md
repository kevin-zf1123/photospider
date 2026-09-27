---
spec_schema_version: 1
id: RES-07
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-07A
- RES-07B
- RES-07C
- RES-07D
- RES-07E
- RES-07F
---

# RES-07: Variance Stabilization and Inverse Transforms

Forward transforms, algebraic inverses, and mean-domain bias-aware inverses are separate members. Inverting an arbitrary denoiser output is not claimed to be statistically unbiased.

This specification defines the intended mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member has been marked Accepted or as a production implementation.

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-07A | [Anscombe forward transform](RES-07A_anscombe_forward.md) | primitive | E |
| RES-07B | [Algebraic Anscombe inverse](RES-07B_anscombe_inverse_algebraic.md) | primitive | E |
| RES-07C | [Poisson mean-domain inverse](RES-07C_anscombe_inverse_mean.md) | primitive | E |
| RES-07D | [Generalized Anscombe forward transform](RES-07D_generalized_anscombe_forward.md) | primitive | E |
| RES-07E | [Generalized algebraic inverse](RES-07E_generalized_anscombe_inverse_algebraic.md) | primitive | E |
| RES-07F | [Poisson–Gaussian mean-domain inverse](RES-07F_generalized_anscombe_inverse_mean.md) | primitive | E |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. A primitive may propose strict and accelerated profiles; a helper only orchestrates explicit stages.

## Shared Mathematics, Units, and Profiles


### Observation Units
Poisson count P has mean λ≥0. Anscombe input is a count, allowing finite noninteger nonnegative measurements. Generalized observations follow y=aP+b+N(0,s²), where a>0 is gain, b is offset, and s≥0 is read_sigma; the stabilized output is dimensionless. The generalized forward transform explicitly applies max(.,0) to a negative radicand and does not reject a Gaussian noisy observation merely because it may be negative.
A mean-domain inverse assumes z estimates E[stabilizer(Y)|λ]; it is not an exact inverse of a single stabilized observation. For Poisson data, m(λ)=Σ_(k≥0) e^-λ λ^k/k! * 2sqrt(k+3/8). For the generalized transform, let ρ=s/a and define mρ(λ)=Σ Poisson(k;λ) ∫ φ(t)*2sqrt(max(k+3/8+ρ²+ρt,0))dt. Solve for λ≥0; return λ=0 when z≤mρ(0), and the unique monotone root when z>mρ(0) (s=0 reduces to the Poisson case). RES-07C and RES-07F may be registered only after strict validation of probability/integral tail bounds, root brackets, and final correct rounding. High-precision agreement alone is insufficient. Resource exhaustion must fail explicitly; do not fall back to an algebraic inverse.


## Shared Execution and Numeric Obligations

E denotes evaluation of the exact expression followed by one final RN_t; B denotes explicitly generated, fixed coefficients; S denotes explicitly staged rounding. See the [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Members must still validate their actual parameter, control-model, and solution-domain constraints. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable; validating payloads of an unrequested component must not introduce a hidden whole-image dependency.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
