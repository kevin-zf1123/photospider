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

The observation model, finite forward operator, true adjoint, native sensitivity, and each per-iteration rounding stage define the profile. Do not substitute an infinite-domain convolution or an arbitrary reflected filter.

Inherit the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

Image inputs use distinct static Int64 `y_axis` and `x_axis`; all other axes are independent batch/channel planes.

| ID | Profile | Type | Numeric behavior |
| --- | --- | --- | --- |
| RES-09A | [Poisson model, no epsilon, fixed steps](RES-09A_richardson_lucy_poisson.md) | primitive | F |
| RES-09B | [Explicit epsilon stabilization, fixed steps](RES-09B_richardson_lucy_stabilized.md) | primitive | F |
| RES-09C | [Poisson model, asynchronous early stop](RES-09C_richardson_lucy_poisson_early_stop.md) | primitive | F |
| RES-09D | [Stabilized model, asynchronous early stop](RES-09D_richardson_lucy_stabilized_early_stop.md) | primitive | F |

## Interface

### Ports and forward model

Inputs are observation `y`, PSF `K`, initial estimate `x0`, background `b`, and mask `m`; outputs are `estimate` and UInt8 `unobserved`. Image-like inputs have the same H/W. Explicit scalar broadcasting for background and mask is allowed only on declared axes. Require nonnegative PSF taps and positive native sum; normalize the PSF in the input dtype before defining the finite convolution matrix `A`. Boundary is explicitly selected from `constant0` or `wrap`; kernel anchor is mandatory. The true `Aᵀ` is obtained by scattering every tap over the actual input/output domain. Reversing the PSF under the same boundary rule is not generally the correct adjoint. Let native sensitivity `s=Aᵀm`. `unobserved[i]=1` iff no selected observation is connected to i by a positive coefficient of the finite native operator. Determine that structural support from coefficients and mask, not from whether a rounded sum underflows to zero. Callers guarantee representable positive sensitivity for structurally observed samples.

Poisson-model values have their stated nonnegative domain when finite. NaN/Inf arithmetic propagates through native operations; do not impose a blanket all-data-finite scan. The stated nonnegative numerical/model domains are unchecked caller premises. Mask values are in `[0,1]`. Bypass/unrequested inputs are not read and create no numerical-validation demand; static shape, schema and parameter-type construction remains independent of those payloads.

## Mathematics

### Recurrence

Initialize from `x0` converted to the main input dtype `t`. For each complete synchronous iteration, use the family’s finite forward operator and true transpose:

1. `v = RN_t(A*x + b)`.
2. At `m_i=0`, set `r_i=+0` without reading `y_i`. Otherwise use `r=RN_t(m*y/(v+epsilon))`. In the no-epsilon profile, if the computed native denominator is zero, define `r=0` for `y=0` and report the explicitly defined no-solution outcome `InvalidDomain` for `y>0` on otherwise valid model inputs.
3. `q = RN_t(Aᵀ*r)`.
4. Update `x_new[i] = RN_t(x_i*q_i/s_i)` on structurally observed support; on unobserved support bit-copy `x_i`.

Compute and reuse `s=Aᵀm` in native input precision; use the same normalized coefficients and finite transpose as the forward model. A rounded zero on structurally observed support violates the representability premise, and must not silently change the unobserved map. Each update is Jacobi/synchronous. There is no per-step flux renormalization. Do not clip output or replace a negative result with zero; a negative result indicates an invalid numerical result under the member contract. Ordinary iterative state follows main input dtype `t`, with direct RN_t publication.

Fixed-step members require explicit `max_iterations>=0`; zero steps returns `initial` by bit-copy for an estimate-only request. An unobserved-only request still consumes PSF and mask. Do not silently early-stop. RES-09B requires explicit finite `epsilon>0` in observation units; RES-09A fixes epsilon at zero and has no hidden floor control. All other caller parameters, including boundary and anchor, are explicit and have no constructor defaults.

## Behavior

The estimate reads its complete plane and the PSF. An unobserved-only selected computation does not require observation, background or initial-estimate arithmetic. A zero-step estimate-only path preserves initial bits. Selected outputs describe one coherent algorithmic state. FFT evaluation, where used, must preserve the exact finite operator and declared stages rather than switching to circular convolution.

## Acceptance

Verify the finite operator dot-product identity within dtype-aware reduction error `<Ax,z>=<x,Aᵀz>`, delta PSF with zero background and unit mask, both one-step formulas, all-zero mask copying the estimate with `unobserved=1`, non-normalized boundary sensitivity, zero-prediction/positive-observation error, and masked nonfinite observation not read. Early-stop predicates follow the [FILTER iterative contract](FILTER_iterative_contract.md).

## Exceptional ratio branch

Under the caller-supplied control/model premises, a zero mask excludes observation arithmetic.
For a positive mask, a NaN observation or prediction propagates to a NaN ratio; it is neither the finite y=0 branch nor the finite y>0 no-solution branch. No NaN payload priority is promised. Valid finite zero denominators retain the explicit zero/positive observation rules above.

## References

[S22 · scikit-image restoration](../research-sources.md#s22).
