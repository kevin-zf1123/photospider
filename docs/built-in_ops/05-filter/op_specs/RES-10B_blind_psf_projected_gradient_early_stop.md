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
- restoration.blind_psf_projected_gradient_early_stop_strict
numeric_reference: S; asynchronous stopping profile
oracle_scope: mathematical_reference
---

# RES-10B: Blind image/PSF estimation with asynchronous early stopping

This is an independent early-stopping profile. It does not add a stop-mode switch to the fixed-step member. Inherit the full input/output, shape, boundary, recurrence, numeric and resource contract from [RES-10](RES-10_contract.md) and the corresponding fixed-step member [RES-10A_blind_psf_projected_gradient.md](RES-10A_blind_psf_projected_gradient.md). Require explicit `y_axis`, `x_axis`, `anchor_y`, `anchor_x`, `max_iterations`, check interval, `epsilon_x`, `epsilon_k`, `step_image`, `step_psf`, `image_l2` and `psf_l2`; factories have no defaults. Boundary is fixed to wrap.

## Recurrence and state

Use RES-10A’s alternating projected-gradient updates and joint objective E. Evaluate both residuals after a complete alternating iteration, at the same image/PSF pair. Keep the PSF simplex state as exact rational data.

Image, residual and gradient states follow main-input dtype `t`. The PSF remains an exact-rational simplex state and its public values are directly RN_t. Fixed-step profiles never stop early; this is an independent profile with an explicit `max_iterations` bound.

## Stopping criterion

After a complete alternating update, evaluate both exact projected-gradient residuals at the same `(x,k)` pair using `eta_x=step_image` and `eta_k=step_psf`. The gradients are recomputed at that pair. Stop only if both `max_i abs(G_x[i])<=epsilon_x` and `max_(j in support) abs(G_k[j])<=epsilon_k`, as defined in the shared iterative contract. Thresholds are explicit, finite, nonnegative and have their separate residual units. The check does not mutate the solver state and does not prove global optimality or PSF uniqueness.

## Asynchronous lifecycle

Use [FILTER iterative contract](FILTER_iterative_contract.md) for the exact paired-state gradient predicate, check scheduling, coherent ownership, dropped busy triggers, termination races, cancellation, resource admission and cleanup. Do not expose checkpoints, iteration diagnostics, or public stopping-result fields. The result is the accepted state defined by that shared contract; it is not a cached cross-call result.

## Errors and resource limits

Parameter/control-model domains remain member-specific. Ordinary numeric propagation follows the corresponding NUM operation; a non-finite value needed by a stopping check makes that check inconclusive and it does not request convergence. Invalid parameters, resource exhaustion, cancellation, upstream failures and arithmetic errors retain their own failure behavior. Charge retained check states and concurrent work to the execution budget. Do not silently change algorithm, precision, or iteration limit.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior, max-iteration termination, and the shared lifecycle contract. Runtime registration and implementation are not implied by this proposal.
