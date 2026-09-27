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

This family specifies a concrete constrained solver rather than an underspecified “blind sharpen” operation. Inherit the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

| ID | Profile | Type | Numeric behavior |
| --- | --- | --- | --- |
| RES-10A | [Projected-gradient alternating updates, fixed steps](RES-10A_blind_psf_projected_gradient.md) | primitive | S image/gradient stages; exact-rational simplex state |
| RES-10B | [Projected-gradient asynchronous early stop](RES-10B_blind_psf_projected_gradient_early_stop.md) | primitive | S image/gradient stages; exact-rational simplex state |

## Model and state

For a single numerical plane, minimize `E(x,k)=0.5*||K*x-y||^2 + 0.5*lambda_x*||x||^2 + 0.5*lambda_k*||k||^2`, subject to `x>=0`, `k>=0`, `sum(k)=1`, and `k` supported only on the supplied support mask. No automatic multiscale processing is part of the member. The caller supplies image, observation, initial PSF, binary UInt8 PSF support and anchor. Image and observation have equal shape; PSF/support have `[Kh,Kw]`, and support contains at least one selected tap. The selected color group must have alpha removed or be explicitly extracted/composited by the caller.

For any positive iteration count, validate initial image/PSF against their constraints; initial PSF must sum exactly to one and is not silently normalized. PSF iterates remain exact rational values, satisfying exact simplex constraints. Ordinary image/residual/gradient states follow main input dtype `t` and use RN_t at declared stages. Public PSF output is rounded directly from the exact rational state with RN_t; do not first round through RN64 or renormalize the published PSF. Consequently, the sum of rounded public taps need not equal exactly one. The rounded public PSF is not an internal continuation checkpoint.

## Profile behavior

Fixed-step RES-10A requires explicit `max_iterations>=0`, `step_image>0`, `step_psf>0`, `image_l2>=0`, `psf_l2>=0`, explicit `y_axis`, `x_axis`, `anchor_y`, and `anchor_x`, and fixed wrap boundary. All controls are required with no constructor defaults. Zero steps bit-copy the initial outputs. No convergence or PSF uniqueness is promised.

RES-10B uses the same recurrence and separate early-stop profile. Both projected-gradient residuals at the same complete image/PSF pair must meet their own explicit finite nonnegative tolerances. Use the objective gradients recomputed at that pair and reuse `step_image` and `step_psf` as check step sizes. Details of the test and asynchronous lifecycle are defined in the member and [FILTER iterative contract](FILTER_iterative_contract.md).

## Numeric and resource obligations

The image and gradient stages have their declared per-stage rounding. Exact rational PSF projection entails numerator/denominator limb growth, which is charged to execution resources. Strict resource shortage is a failure; do not lower precision, omit a projection, silently adapt step sizes, or change the algorithm. Work is `O(T*P*C*A)` plus `O(A log A)` for each support simplex sort, where T is iteration count, P image pixels, C planes (this profile requires C=1), and A PSF taps. Sorting and projection ties are deterministic; ties in sorted values use original tap order.

Whole image and PSF are needed; support is control data. Image and PSF outputs belong to the same iteration result. Errors do not publish partial outputs. Cancellation, ownership and budgets follow FILTER common contracts. Ordinary numerical NaN/Inf/overflow behavior follows NUM, while parameter, support, initialization and constrained-model validation remain explicit.

## Acceptance

Check exact support/nonnegativity/sum-one constraints, a hand-computed simplex projection (`[0.2,-0.1,0.9] -> [0.15,0,0.85]`), a fixed point, degenerate/multiple-solution cases, distinct initializations, all requested output combinations, resource failure and cancellation. Early-stop fixtures alone do not validate its concurrent lifecycle; use the acceptance matrix in FILTER iterative contract.

## References

[S22 · scikit-image restoration](../research-sources.md#s22).

## Exceptional PSF projection state

Exact-rational simplex feasibility is guaranteed for finite projection arguments.
A NaN/Inf argument has no finite Euclidean simplex projection. Apply NUM invalid
arithmetic propagation instead of converting it to a rational or reporting floating
overflow as failure: represent this exceptional state by a NaN tag and publish the
same quiet NaN at every selected support tap, with +0 outside support. Choose the
first NaN argument in original support tap order, preserving its NUM sign/payload;
if there is only infinity, generate the fixed positive quiet NaN. This tag is not
a feasible simplex certificate and does not claim sum-one. Later computations
consume those exceptional components under NUM; the exceptional PSF remains tagged.
No early-stop check can pass on that state. Fixed-step execution still completes
its declared count, and early-stop execution can end at max_iterations. Required
initial finite PSF/domain validation, resource errors and cancellation remain errors.
The finite constrained state still uses exact rationals without intermediate Float64
rounding. `special.exceptional_simplex` covers the projection branch, not a complete
concurrent solver implementation.
