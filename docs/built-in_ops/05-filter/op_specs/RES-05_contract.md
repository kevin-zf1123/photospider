---
spec_schema_version: 1
id: RES-05
kind: family_contract
category: 05-filter
status: Proposed
implementation_status: implemented
parent_id: 05-filter
members:
- RES-05A
- RES-05B
- RES-05C
- RES-05D
---

# RES-05: ROF total-variation denoising

The objective, boundary, solver and stopping rule jointly define each profile. A library “weight” is not interchangeable with this contract’s lambda.

Inherit the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). Image fields bind distinct static Int64 `y_axis` and `x_axis` values as defined by the FILTER common contract; all remaining axes are independent batch/channel planes.

## Members

| ID | Specification | Type | Numeric profile |
| --- | --- | --- | --- |
| RES-05A | [Isotropic ROF, fixed steps](RES-05A_tv_rof_isotropic.md) | primitive | F |
| RES-05B | [Anisotropic ROF, fixed steps](RES-05B_tv_rof_anisotropic.md) | primitive | F |
| RES-05C | [Isotropic ROF, asynchronous early stop](RES-05C_tv_rof_isotropic_early_stop.md) | primitive | F |
| RES-05D | [Anisotropic ROF, asynchronous early stop](RES-05D_tv_rof_anisotropic_early_stop.md) | primitive | F |

The suffix identifies a distinct mathematical/interface object, not a backend-selected quality tier. Primitive profiles use native fp32/fp64 arithmetic; helpers compose explicit algorithmic stages.

## Shared objective and fixed-step recurrence

For each independent plane, minimize `E(u)=0.5*Σ(u-f)^2 + λ*Σ φ((K_y u),(K_x u))`. `K` is the forward-difference operator. A difference beyond the image domain is zero (no-flux). `Kᵀ` is the true transpose of this finite-domain matrix, not an arbitrary reflected convolution. `spacing_x` and `spacing_y` are explicit and enter the corresponding difference operator. Each member defines `φ` and its dual projection.

Initialize `u_0=f`, `ubar_0=f`, and `p_0=(0,0)`. A complete Jacobi iteration is synchronous at every pixel; Gauss–Seidel pointwise updates are not equivalent. The ordinary primal, extrapolated and dual states use main-input dtype `t`, with RN_t at the declared stages. Invalid forward-boundary dual components are fixed at +0. Parameters satisfy `tau>0`, `sigma>0`, `theta∈[0,1]`, and `tau*sigma*4*(1/dx^2+1/dy^2)<1`; this is the stated conservative stability bound. Fixed-step profiles take explicit `max_iterations>=0`. For `lambda=0` or `max_iterations=0`, return an input bit-copy. Fixed-step members never exit early based on data. Energy and residual diagnostics are not public result ports.

## Numeric Requirements

Ordinary arithmetic and stored state follow [RES native floating-point contract](RES_native_contract.md). Caller domain premises remain unchecked. Copy rules, support and discrete decisions remain normative, evaluated on native working values.

The member's support is normative. A halo does not imply independent evaluation on arbitrary clipped ROIs. Preserve the declared rounding stages, global dependencies and iterative state.

See [FILTER color composition](FILTER_color_composition.md), [boundary contract](FILTER_boundary_contract.md), [collections contract](FILTER_collections_contract.md), and [numeric reference](FILTER_numeric_reference.md).

## Acceptance

Members are accepted against the [FILTER oracle protocol](FILTER_oracle_protocol.md). The early-stop members additionally follow the [iterative execution contract](FILTER_iterative_contract.md).

## References

[S19 · Getreuer: ROF TV using Split Bregman](../research-sources.md#s19); [S22 · scikit-image restoration](../research-sources.md#s22). Sources motivate algorithm families but do not set this project’s parameter defaults, schemas or licenses.
