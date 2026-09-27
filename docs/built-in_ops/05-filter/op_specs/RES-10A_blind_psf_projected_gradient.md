---
spec_schema_version: 1
id: RES-10A
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-10
function: blind_psf_projected_gradient
proposed_operation_keys:
- restoration.blind_psf_projected_gradient_strict
- restoration.blind_psf_projected_gradient_accelerated_apple_silicon
- restoration.blind_psf_projected_gradient_accelerated_x86_64
numeric_reference: S image/gradient stages + exact-rational simplex state
oracle_scope: mathematical_reference
---

# RES-10A: Blind PSF projected-gradient estimation, fixed steps

Inherit the [RES-10 family contract](RES-10_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Ports and shape

`observation, initial_image, initial_psf, support` → `estimate, psf`. `support` is UInt8 `[Kh,Kw]` with at least one selected element. Image and observation have the same H/W and each is a single plane. The PSF has `[Kh,Kw]` and is published in main-input dtype `t`. This is a raw numerical operation, not automatically a color or alpha-aware operation.

## Required parameters and domain

Require explicit `max_iterations>=0`, `step_image=eta_x>0`, `step_psf=eta_k>0`, `image_l2=lambda_x>=0`, `psf_l2=lambda_k>=0`, `y_axis`, `x_axis`, `anchor_y`, and `anchor_x`; boundary is fixed to wrap. Constructors have no defaults. For any positive iteration count, `initial_image>=0` and `initial_psf>=0`, the initial PSF must be supported only on the selected footprint and have exact sum one. Do not normalize a violating initial PSF. The image, observation, support and parameters are validated before iteration as required by the family; NUM propagation applies to ordinary arithmetic values.

## Objective and recurrence

Minimize `0.5*||A_k*x-y||^2 + 0.5*lambda_x*||x||^2 + 0.5*lambda_k*||k||^2`, with `x>=0` and `k` on the supported unit simplex. Initialize from the supplied image and exact-rational PSF. At each complete alternating step:

1. `r=RN_t(A_k*x-y)`.
2. `x_new=RN_t(max(0,x-eta_x*(A_k^T*r+lambda_x*x)))`.
3. Recompute `r2=RN_t(A_(k)*x_new-y)`.
4. `g_k=RN_t(B_(x_new)^T*r2+lambda_k*k)`, where `B_x` is the PSF-gradient operator defined by the wrapped convolution model.
5. Project exact `v=k-eta_k*g_k` onto the unit simplex restricted to support. Sort descending, breaking ties by original tap order. Let `rho` be the largest index satisfying `v_(j)>(sum_(i<=j)v_i-1)/j`, `theta=(sum_(i<=rho)v_i-1)/rho`; then `k_new[i]=max(v_i-theta,0)` on support and zero outside it.

Keep `k_new` as exact rational state so `sum(k)=1` remains exact. Do not write it back to Float64 between iterations. Publish estimate with RN_t and each PSF tap directly from exact rational state with RN_t. Do not normalize the rounded output PSF. A zero-iteration call returns the supplied initial image and PSF according to the output conversion contract. Fixed-step execution does not claim convergence or a unique PSF.

## Demand, resources and failure

Whole image and PSF are required; support is control data. Both outputs refer to one coherent iteration state. Descriptor shape is derived from inputs and static parameters. Work is `O(T*P*A)+O(T*A log A)` for this single-plane member. Charge exact rational limb growth and projection workspace. Budget exhaustion fails without changing precision or publishing a partial pair. Cancellation and owner lifetime follow FILTER common contracts.

## Oracle and acceptance

The oracle function `restoration.blind_psf(observation, initial_image, initial_psf, support, *, iterations=1, step_image=0.1, step_psf=0.01, image_l2=0, psf_l2=0, anchor=(0,0), dtype="float64")` is in [restoration.py](../../../../oracle/ops/filter/oracles/restoration.py). Its `iterations` argument corresponds to the proposed public `max_iterations` parameter; oracle tuple `anchor=(y,x)` maps from public `anchor_y` and `anchor_x`. The `simplex_exact_projection` fixture covers `ExactRationalInternalState`; `blind_psf_fixed_point` covers the fixed-point formula using `ExactRationalStaged`. Check exact simplex invariants, `[0.2,-0.1,0.9] -> [0.15,0,0.85]`, degenerate/multiple solutions, outputs’ shared state, zero iterations, resource failure and cancellation. Fixtures validate the reference only; see the [oracle README](../../../../oracle/ops/filter/README.md).

The proposed strict and CPU accelerated keys are not registered. Accelerated execution must preserve exact tie/constraint decisions and meet NUM’s final FP32-scaled four-ULP guarantee with fallback as needed. No implementation or performance evidence is asserted.

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
