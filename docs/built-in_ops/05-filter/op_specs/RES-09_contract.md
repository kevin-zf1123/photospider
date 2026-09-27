---
spec_schema_version: 1
id: RES-09
kind: family_contract
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-09A
- RES-09B
- RES-09C
- RES-09D
---

# RES-09: Richardson–Lucy restoration

The observation model, finite forward operator, true adjoint, exact sensitivity, and each per-iteration rounding stage define the profile. Do not substitute an infinite-domain convolution or an arbitrary reflected filter.

Inherit the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

Image inputs use distinct static Int64 `y_axis` and `x_axis`; all other axes are independent batch/channel planes.

| ID | Profile | Type | Numeric behavior |
| --- | --- | --- | --- |
| RES-09A | [Poisson model, no epsilon, fixed steps](RES-09A_richardson_lucy_poisson.md) | primitive | S |
| RES-09B | [Explicit epsilon stabilization, fixed steps](RES-09B_richardson_lucy_stabilized.md) | primitive | S |
| RES-09C | [Poisson model, asynchronous early stop](RES-09C_richardson_lucy_poisson_early_stop.md) | primitive | S |
| RES-09D | [Stabilized model, asynchronous early stop](RES-09D_richardson_lucy_stabilized_early_stop.md) | primitive | S |

## Ports and forward model

Inputs are observation `y`, PSF `K`, initial estimate `x0`, background `b`, and mask `m`; outputs are `estimate` and UInt8 `unobserved`. Image-like inputs have the same H/W. Explicit scalar broadcasting for background and mask is allowed only on declared axes. Require nonnegative PSF taps and exact positive sum; normalize the PSF exactly before defining the finite convolution matrix `A`. Boundary is explicitly selected from `constant0` or `wrap`; kernel anchor is mandatory. The true `Aᵀ` is obtained by scattering every tap over the actual input/output domain. Reversing the PSF under the same boundary rule is not generally the correct adjoint. Let exact sensitivity `s=Aᵀm`; `unobserved[i]=1` iff `s_i=0`.

Poisson-model values have their stated nonnegative domain when finite. NUM-permitted NaN/Inf arithmetic propagates according to NUM; do not impose a blanket all-data-finite scan. Negative values that violate the selected Poisson/control model are `InvalidDomain`. Mask values are in `[0,1]`. Bypass/unrequested inputs are not read except for required shape, structure, parameter and descriptor validation.

## Recurrence

Initialize from `x0` converted to the main input dtype `t`. For each complete synchronous iteration, use the family’s exact forward operator and true transpose:

1. `v = RN_t(A*x + b)`.
2. At `m_i=0`, set `r_i=+0` without reading `y_i`. Otherwise use `r=RN_t(m*y/(v+epsilon))`. In the no-epsilon profile, if the exact denominator is zero, define `r=0` for `y=0` and fail `InvalidDomain` for `y>0`.
3. `q = RN_t(Aᵀ*r)`.
4. Update `x_new[i] = RN_t(x_i*q_i/s_i)` when `s_i>0`; if `s_i=0`, bit-copy `x_i`.

Use exact `s=Aᵀm`; do not round `s` to Float64 before division. Each update is Jacobi/synchronous. There is no per-step flux renormalization. Do not clip output or replace a negative result with zero; a negative result indicates an invalid numerical result under the member contract. Ordinary iterative state follows main input dtype `t`, with direct RN_t publication.

Fixed-step members require explicit `max_iterations>=0`; zero steps returns `initial` by bit-copy for an estimate-only request. An unobserved-only request still consumes PSF and mask. Do not silently early-stop. RES-09B requires explicit finite `epsilon>0` in observation units; RES-09A fixes epsilon at zero and has no hidden floor control. All other caller parameters, including boundary and anchor, are explicit and have no constructor defaults.

## Demand, resources and errors

The estimate requests Whole for its plane and full PSF. An unobserved-only request does not read observation, background or initial estimate. A zero-step estimate-only request bit-copies initial; PSF and mask still undergo descriptor/preflight checks as needed. If estimate is not requested, do not read unused observation/background/initial payloads beyond required parameter/model validation. Both outputs share a coherent result owner. The descriptor and mask/PSF structural requirements can be validated independently of image samples.

The direct reference is `O(max_iterations*P*C*A)` and retains `A`, `Aᵀ` support/plan and `s`; FFT replacement must preserve the exact finite operator and declared stages, not switch to circular convolution. Charge all operator plans, state buffers and work. Resource exhaustion is an explicit failure. No implicit spill, reduced precision, fallback algorithm or partial output. Cancellation and ownership follow shared FILTER contracts.

## Acceptance

Verify the exact dot-product identity `<Ax,z>=<x,Aᵀz>`, delta PSF with zero background and unit mask, both one-step formulas, all-zero mask copying the estimate with `unobserved=1`, non-normalized boundary sensitivity, zero-prediction/positive-observation error, masked nonfinite observation not read, and independent output requests. Early-stop predicates, scheduling, owner pinning, race handling and cleanup follow [FILTER iterative contract](FILTER_iterative_contract.md).

## References

[S22 · scikit-image restoration](../research-sources.md#s22).

### Exceptional ratio branch

After required control/model validation, a zero mask excludes observation arithmetic.
For a positive mask, an observation NaN propagates with NUM operand priority even
when the prediction denominator is zero; it is neither the y=0 branch nor the y>0
domain-error branch. A prediction NaN also propagates when no earlier numerator NaN
has priority. Valid finite zero denominators retain the explicit zero/positive
observation rules above. Scalar fixtures are provided by `special.rl_ratio`;
complete exceptional solver trajectories remain outside the finite-rational oracle.
