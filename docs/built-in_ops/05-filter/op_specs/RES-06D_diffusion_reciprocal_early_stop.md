---
spec_schema_version: 1
id: RES-06D
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-06
function: diffusion_reciprocal_early_stop
proposed_operation_keys:
- restoration.diffusion_reciprocal_early_stop_strict
numeric_reference: S; asynchronous stopping profile
oracle_scope: mathematical_reference
---

# RES-06D: Reciprocal edge-preserving diffusion with asynchronous early stopping

This is an independent early-stopping profile. It does not add a stop-mode switch to the fixed-step member. Inherit the full input/output, shape, boundary, recurrence, numeric and resource contract from [RES-06](RES-06_contract.md) and the corresponding fixed-step member [RES-06B_diffusion_reciprocal.md](RES-06B_diffusion_reciprocal.md). Every algorithm control, `y_axis`, `x_axis`, `max_iterations`, `rate_tolerance`, and check interval is mandatory; constructors have no defaults.

## Recurrence and state

Use RES-06B’s synchronous explicit diffusion recurrence, no-flux boundary and reciprocal conductance `c_ij=RN64(1/(1+(d_ij/kappa)^2))`; preserve the ordinary state in the main input dtype.

The recurrence state follows main input dtype `t`, and the final result is rounded directly to `t` with RN_t. Fixed-step profiles never stop early; this member has a distinct explicit `max_iterations` bound.

## Stopping criterion

At a check compute `F_i(u)=sum_j c_ij*(u_j-u_i)/spacing_ij^2` from the current complete state and baked conductances. Stop when `max_i abs(F_i(u)) <= rate_tolerance`. `rate_tolerance` is mandatory, finite, nonnegative, and measured in value-units per diffusion-time. Evaluate the defined rate directly, without using a rounded state delta divided by `dt`. The condition is a model-rate criterion and does not promise best denoising quality.

## Asynchronous lifecycle

Use the shared [FILTER iterative contract](FILTER_iterative_contract.md) for scheduling, coherent state ownership, dropped busy triggers, termination races, cancellation, cleanup, and check-held resource admission. Do not expose checkpoints, iteration diagnostics, or public stopping-result fields. The result is the accepted state defined by that shared contract; it is not a cached cross-call result.

## Errors and resource limits

Parameter/control-model domains remain member-specific. Ordinary numeric propagation follows the corresponding NUM operation; a non-finite value needed by a stopping check makes that check inconclusive and it does not request convergence. Invalid parameters, resource exhaustion, cancellation, upstream failures and arithmetic errors retain their own failure behavior. Charge retained check states and concurrent work to the execution budget. Do not silently change algorithm, precision, or iteration limit.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior, max-iteration termination, and the shared lifecycle contract. Runtime registration and implementation are not implied by this proposal.
