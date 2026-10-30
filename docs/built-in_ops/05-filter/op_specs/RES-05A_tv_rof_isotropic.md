---
spec_schema_version: 1
id: RES-05A
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-05
function: tv_rof_isotropic
proposed_operation_keys:
- restoration.tv_rof_isotropic
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-05A: Isotropic ROF denoising, fixed steps

Inherit the [RES-05 family contract](RES-05_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). This is a raw numerical-field operation: it processes each plane independently and does not infer RGB, vector or alpha semantics from channel count or attached color metadata.

## Interface

Operation key: `restoration.tv_rof_isotropic`.

### Ports

`input` and `output` are Results with matching fixed Tensor schemas and one independent solve per plane. The operation does not implement vector/color TV. Color interpretation, if needed by a surrounding workflow, is explicit configuration rather than attached Result metadata.

### Parameters

`lambda>=0`, `max_iterations>=0`, `tau>0`, `sigma>0`, `theta∈[0,1]`, `dx>0`, `dy>0`, and explicit `y_axis` and `x_axis` are required. Also require `tau*sigma*4*(1/dx^2+1/dy^2)<1`. There are no constructor defaults. `lambda` is in the units of the stated ROF objective; `dx,dy` are sample spacings. Real values mean their stored Float64 values, not exact decimal rationals. Fixed mathematical choices are the no-flux boundary and this member’s named `phi`; configurable parameters may not be omitted.

## Mathematics

For each plane minimize `0.5*Σ(u-f)^2 + lambda*Σ phi(K_y u,K_x u)`, with `phi(a,b)=sqrt(a^2+b^2)`. Initialize `u=f`, `ubar=f`, and `p=(0,0)`. At each complete synchronous Jacobi step:

1. `p_hat = p + sigma*K*ubar`, with the declared native state-store stage for each component.
2. `p_new = RN_t(p_hat/max(1,sqrt(p_hat_x^2+p_hat_y^2)/lambda))`.
3. `u_new = RN_t((u - tau*K^T*p_new + tau*f)/(1+tau))`.
4. `ubar_new = RN_t(u_new + theta*(u_new-u))`.

`K` is the finite-domain forward difference with invalid boundary differences zero; `K^T` is its exact matrix transpose. The member’s boundary and stored-state stages are part of the algorithm. A fixed number of steps does not claim an exact minimizer. Do not clip output to the input range. For `lambda=0` or `max_iterations=0`, return an input bit-copy. No implicit early exit is allowed.

## Acceptance

Check the zero-lambda and zero-step bit-copy branches, constant preservation, a hand-computed 2×2 one-step result, and the bilinear identity `<Ku,p>=<u,K^T p>` within native-precision roundoff. The finite-grid adjoint must be the exact algebraic transpose, including boundary contributions; separately rounded inner products need not be bitwise equal. Account for accumulation length and absolute term magnitudes when checking cancellation-prone dot products. Isotropic TV does not claim rotational invariance under grid rotation. Do not require the objective to decrease monotonically at every Chambolle–Pock step.
