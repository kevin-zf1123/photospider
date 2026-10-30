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

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-07A | [Anscombe forward transform](RES-07A_anscombe_forward.md) | primitive | F |
| RES-07B | [Algebraic Anscombe inverse](RES-07B_anscombe_inverse_algebraic.md) | primitive | F |
| RES-07C | [Poisson mean-domain inverse](RES-07C_anscombe_inverse_mean.md) | primitive | F |
| RES-07D | [Generalized Anscombe forward transform](RES-07D_generalized_anscombe_forward.md) | primitive | F |
| RES-07E | [Generalized algebraic inverse](RES-07E_generalized_anscombe_inverse_algebraic.md) | primitive | F |
| RES-07F | [Poisson–Gaussian mean-domain inverse](RES-07F_generalized_anscombe_inverse_mean.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

## Mathematics

### Observation Units
Poisson count P has mean λ≥0. Anscombe input is a count, allowing finite noninteger nonnegative measurements. Generalized observations follow y=aP+b+N(0,s²), where a>0 is gain, b is offset, and s≥0 is read_sigma; the stabilized output is dimensionless. The generalized forward transform explicitly applies max(.,0) to a negative radicand and does not reject a Gaussian noisy observation merely because it may be negative.
A mean-domain inverse assumes z estimates E[stabilizer(Y)|λ]; it is not an exact inverse of a single stabilized observation. For Poisson data, m(λ)=Σ_(k≥0) e^-λ λ^k/k! * 2sqrt(k+3/8). For the generalized transform, let ρ=s/a and define mρ(λ)=Σ Poisson(k;λ) ∫ φ(t)*2sqrt(max(k+3/8+ρ²+ρt,0))dt. Solve for λ≥0; return λ=0 when z≤mρ(0), and the unique monotone root when z>mρ(0) (s=0 reduces to the Poisson case). RES-07C and RES-07F require documented probability/integral truncation and root tolerances, monotone branch handling, and independent high-precision error tests over their supported range. Native fp32/fp64 accuracy is required; exact final rounding is not required.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)
