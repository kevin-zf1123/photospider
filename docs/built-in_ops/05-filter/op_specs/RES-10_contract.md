---
spec_schema_version: 1
id: RES-10
kind: family_contract
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-10A
- RES-10B
---

# RES-10: Joint blind image and PSF estimation

This family specifies a concrete constrained solver rather than an underspecified “blind sharpen” operation. Inherit the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

| ID | Profile | Type | Numeric behavior |
| --- | --- | --- | --- |
| RES-10A | [Projected-gradient alternating updates, fixed steps](RES-10A_blind_psf_projected_gradient.md) | primitive | F |
| RES-10B | [Projected-gradient asynchronous early stop](RES-10B_blind_psf_projected_gradient_early_stop.md) | primitive | F |

## Model and state

For a single numerical plane, minimize `E(x,k)=0.5*||K*x-y||^2 + 0.5*lambda_x*||x||^2 + 0.5*lambda_k*||k||^2`, subject to `x>=0`, `k>=0`, `sum(k)=1`, and `k` supported only on the supplied support mask. No automatic multiscale processing is part of the member. The caller supplies image, observation, initial PSF, binary UInt8 PSF support and anchor. Image and observation have equal shape; PSF/support have `[Kh,Kw]`, and support contains at least one selected tap. The selected color group must have alpha removed or be explicitly extracted/composited by the caller.

For any positive iteration count, callers guarantee the initial image/PSF constraints; initial PSF must satisfy finite positive native mass with abs(sum(initial_psf)-1)<=min(32*epsilon_t*A,1/1024), where epsilon_t is machine epsilon at 1 and A counts selected support taps and is not silently normalized. PSF iterates, image/residual/gradient states and public outputs use the input dtype. Support zeros and nonnegativity remain exact. Unit sum is subject to native projection roundoff and is validated with a scale- and support-size-aware tolerance; it is not an exact-rational invariant. Do not silently renormalize either initial or published PSF.

## Profile behavior

Fixed-step RES-10A requires explicit `max_iterations>=0`, `step_image>0`, `step_psf>0`, `image_l2>=0`, `psf_l2>=0`, explicit `y_axis`, `x_axis`, `anchor_y`, and `anchor_x`, and fixed wrap boundary. All controls are required with no constructor defaults. Zero steps bit-copy the initial outputs. No convergence or PSF uniqueness is promised.

RES-10B uses the same recurrence and separate early-stop profile. Both projected-gradient residuals at the same complete image/PSF pair must meet their own explicit finite nonnegative tolerances. Use the objective gradients recomputed at that pair and reuse `step_image` and `step_psf` as check step sizes. Details of the test and asynchronous lifecycle are defined in the member and [FILTER iterative contract](FILTER_iterative_contract.md).

## Numeric and resource obligations

The image and gradient stages have their declared per-stage rounding. Native PSF projection requires sorting and prefix-sum workspace, charged to execution resources. Resource shortage is a failure; do not lower precision, omit a projection, silently adapt step sizes, or change the algorithm. Work is `O(T*P*C*A)` plus `O(A log A)` for each support simplex sort, where T is iteration count, P image pixels, C planes (this profile requires C=1), and A PSF taps. Sorting and projection ties are deterministic; ties in sorted values use original tap order.

complete-input image and PSF are needed; support is control data. Image and PSF outputs belong to the same iteration result. Errors do not publish partial outputs. Cancellation, ownership and budgets follow FILTER common contracts. Ordinary numerical NaN/Inf/overflow behavior follows native arithmetic, while parameter, support, initialization and constrained-model numerical domains remain unchecked caller premises.

## Acceptance

Check exact support/nonnegativity and native unit-sum accuracy, a hand-computed simplex projection (`[0.2,-0.1,0.9] -> [0.15,0,0.85]`), a fixed point, degenerate/multiple-solution cases, distinct initializations, all requested output combinations, resource failure and cancellation. Early-stop fixtures alone do not validate its concurrent lifecycle; use the acceptance matrix in FILTER iterative contract.

## References

[S22 · scikit-image restoration](../research-sources.md#s22).

## Exceptional PSF projection state

Finite simplex projection uses native arithmetic, with error checked against the mathematical Euclidean projection. A non-finite projection argument has no finite Euclidean projection: publish NaN at every selected support tap and +0 outside support. NaN payload/sign is unspecified. This exceptional state is not a feasibility certificate and cannot establish convergence. Fixed-step execution keeps its declared count; resource failure and cancellation retain their defined behavior. Existing rational simplex fixtures are mathematical references, not native bitwise goldens.
