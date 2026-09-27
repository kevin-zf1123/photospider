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
- restoration.tv_rof_isotropic_strict
- restoration.tv_rof_isotropic_accelerated_apple_silicon
- restoration.tv_rof_isotropic_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
---

# RES-05A: Isotropic ROF denoising, fixed steps

Inherit the [RES-05 family contract](RES-05_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). This is a raw numerical-field operation: it processes each plane independently and does not infer RGB, vector or alpha semantics from channel count or attached color metadata.

## Ports and shape

`input` → `output`, with the same shape and one independent solve per plane. The operation does not implement vector/color TV. Publish canonical planar output and preserve or rebuild applicable metadata under the shared contract.

## Required parameters and domain

`lambda>=0`, `max_iterations>=0`, `tau>0`, `sigma>0`, `theta∈[0,1]`, `dx>0`, `dy>0`, and explicit `y_axis` and `x_axis` are required. Also require `tau*sigma*4*(1/dx^2+1/dy^2)<1`. There are no constructor defaults. `lambda` is in the units of the stated ROF objective; `dx,dy` are sample spacings. Real values mean their stored Float64 values, not exact decimal rationals. Fixed mathematical choices are the no-flux boundary and this member’s named `phi`; configurable parameters may not be omitted.

## Mathematical and rounding contract

For each plane minimize `0.5*Σ(u-f)^2 + lambda*Σ phi(K_y u,K_x u)`, with `phi(a,b)=sqrt(a^2+b^2)`. Initialize `u=f`, `ubar=f`, and `p=(0,0)`. At each complete synchronous Jacobi step:

1. `p_hat = p + sigma*K*ubar`, with the declared RN_t stage for each component.
2. `p_new = RN_t(p_hat/max(1,sqrt(p_hat_x^2+p_hat_y^2)/lambda))`.
3. `u_new = RN_t((u - tau*K^T*p_new + tau*f)/(1+tau))`.
4. `ubar_new = RN_t(u_new + theta*(u_new-u))`.

`K` is the finite-domain forward difference with invalid boundary differences zero; `K^T` is its exact matrix transpose. The member’s boundary and stored-state stages are part of the algorithm. A fixed number of steps does not claim an exact minimizer. Do not clip output to the input range. For `lambda=0` or `max_iterations=0`, return an input bit-copy. No implicit early exit is allowed.

## Demand and resource behavior

Each output plane requires Whole input for the fixed-step profile. Although a finite step count admits a theoretical expanded support, this proposal specifies Whole execution and no partial-state sharing. Energy and residual diagnostics are not public result ports. Shape/descriptor inference depends only on parameters and input descriptor.

Work is `O(max_iterations*P*C)`, with dual planes and two generations each of `u`/`ubar`; P is plane pixel count and C plane count. Account for all stages and working buffers. Insufficient budget fails; do not shorten the iteration count, spill implicitly, lower precision, or publish a partial value. Cancellation, ownership and error publication follow FILTER shared contracts.

## Acceptance and oracle

Check the zero-lambda and zero-step bit-copy branches, constant preservation, a hand-computed 2×2 one-step result, and the exact adjoint identity `<Ku,p>=<u,K^T p>`. Isotropic TV does not claim rotational invariance under grid rotation. Do not require the objective to decrease monotonically at every Chambolle–Pock step.

The oracle function is `restoration.tv_cp(image, *, lam, iterations=100, tau=0.24, sigma=0.24, theta=1, dx=1, dy=1, isotropic=True, dtype="float64")` in [restoration.py](../../../../oracle/ops/filter/oracles/restoration.py). Its `iterations` argument corresponds to the proposed public `max_iterations` parameter. The `tv_one_step_True` fixture covers the reference formula using `ExactRationalStaged`; it does not establish runtime support or the full parameter, metadata, ROI, budget or cancellation contract. See the [oracle README](../../../../oracle/ops/filter/README.md).

The proposed strict and CPU accelerated keys are not registered or implemented. Accelerated variants must meet NUM’s final FP32-scaled four-ULP bound and use strict fallback where required. Float64 paths retain Float64 input/output and exponent domain; Float32 out-of-range cases follow NUM fallback. No kernel/CPU/ISA performance or differential conformance is claimed here.
