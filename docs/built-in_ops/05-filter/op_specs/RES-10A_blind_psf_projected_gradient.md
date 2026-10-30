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
- restoration.blind_psf_projected_gradient
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-10A: Blind PSF projected-gradient estimation, fixed steps

Inherit the [RES-10 family contract](RES-10_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation key: `restoration.blind_psf_projected_gradient`.

### Ports

`observation, initial_image, initial_psf, support` → `estimate, psf`. `support` is UInt8 `[Kh,Kw]` with at least one selected element. Image and observation have the same H/W and each is a single plane. The PSF has `[Kh,Kw]` and is published in main-input dtype `t`. This is a raw numerical operation, not automatically a color or alpha-aware operation.

### Parameters

Require explicit `max_iterations>=0`, `step_image=eta_x>0`, `step_psf=eta_k>0`, `image_l2=lambda_x>=0`, `psf_l2=lambda_k>=0`, `y_axis`, `x_axis`, `anchor_y`, and `anchor_x`; boundary is fixed to wrap. Constructors have no defaults. For any positive iteration count, `initial_image>=0` and `initial_psf>=0`, the initial PSF must be supported only on the selected footprint and satisfy finite positive native mass with abs(sum(initial_psf)-1)<=min(32*epsilon_t*A,1/1024), where epsilon_t is machine epsilon at 1 and A counts selected support taps. Do not normalize a violating initial PSF. The image, observation, support and numerical parameters satisfy the family's unchecked caller premises; native propagation applies to ordinary arithmetic values.

## Mathematics

Minimize `0.5*||A_k*x-y||^2 + 0.5*lambda_x*||x||^2 + 0.5*lambda_k*||k||^2`, with `x>=0` and `k` on the supported unit simplex. Initialize from the supplied image and native input-dtype PSF. At each complete alternating step:

1. `r=RN_t(A_k*x-y)`.
2. `x_new=RN_t(max(0,x-eta_x*(A_k^T*r+lambda_x*x)))`.
3. Recompute `r2=RN_t(A_(k)*x_new-y)`.
4. `g_k=RN_t(B_(x_new)^T*r2+lambda_k*k)`, where `B_x` is the PSF-gradient operator defined by the wrapped convolution model.
5. Form native `v=k-eta_k*g_k` and project it onto the unit simplex restricted to support. Sort descending, breaking ties by original tap order. Let `rho` be the largest index satisfying `v_(j)>(sum_(i<=j)v_i-1)/j`, `theta=(sum_(i<=rho)v_i-1)/rho`; then `k_new[i]=max(v_i-theta,0)` on support and zero outside it.

Store k_new in input dtype t. Support zeros and nonnegativity are exact; sum-one holds within native projection roundoff and must be measured. Do not replace the sorted-threshold Euclidean projection with clamp-and-renormalize, and do not renormalize public output. A zero-iteration call returns supplied initial image and PSF according to the output conversion contract. Fixed steps do not claim convergence or uniqueness.

### Exceptional PSF projection state

Finite simplex projection uses native arithmetic, with error checked against the mathematical Euclidean projection. A non-finite projection argument has no finite Euclidean projection: publish NaN at every selected support tap and +0 outside support. NaN payload/sign is unspecified. This exceptional state is not a feasibility certificate and cannot establish convergence. Fixed-step execution keeps its declared count.
