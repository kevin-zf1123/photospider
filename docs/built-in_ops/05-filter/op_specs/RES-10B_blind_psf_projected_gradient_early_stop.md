---
spec_schema_version: 1
id: RES-10B
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-10
function: blind_psf_projected_gradient_early_stop
proposed_operation_keys:
- restoration.blind_psf_projected_gradient_early_stop
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-10B: Blind image/PSF estimation with asynchronous early stopping

This is an independent early-stopping profile, not a stop-mode switch on the fixed-step member. It inherits the input/output, shape, boundary, recurrence and numeric contract from [RES-10](RES-10_contract.md) and the fixed-step member [RES-10A_blind_psf_projected_gradient.md](RES-10A_blind_psf_projected_gradient.md). Arithmetic follows the [RES native floating-point contract](RES_native_contract.md).

## Interface

Operation key: `restoration.blind_psf_projected_gradient_early_stop`.

Require explicit `y_axis`, `x_axis`, `anchor_y`, `anchor_x`, `max_iterations`, check interval, `epsilon_x`, `epsilon_k`, `step_image`, `step_psf`, `image_l2` and `psf_l2`; factories have no defaults. Boundary is fixed to wrap.

## Mathematics

Use RES-10A’s alternating projected-gradient updates and joint objective E. Evaluate both residuals after a complete alternating iteration, at the same image/PSF pair. Keep PSF and image states in the native input dtype.

Image, residual, gradient and PSF states follow main-input dtype `t`; public values are those native states. Fixed-step profiles never stop early; this is an independent profile with an explicit `max_iterations` bound.

### Stopping criterion

After a complete alternating update, evaluate both native projected-gradient residuals at the same `(x,k)` pair using `eta_x=step_image` and `eta_k=step_psf`. The gradients are recomputed at that pair. Stop only if both `max_i abs(G_x[i])<=epsilon_x` and `max_(j in support) abs(G_k[j])<=epsilon_k`, as defined in the shared iterative contract. Thresholds are explicit, finite, nonnegative and have their separate residual units. The check does not mutate the solver state and does not prove global optimality or PSF uniqueness.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior and max-iteration termination.
