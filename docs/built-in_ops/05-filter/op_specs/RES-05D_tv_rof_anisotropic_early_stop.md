---
spec_schema_version: 1
id: RES-05D
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-05
function: tv_rof_anisotropic_early_stop
proposed_operation_keys:
- restoration.tv_rof_anisotropic_early_stop_strict
numeric_reference: S; asynchronous stopping profile
oracle_scope: mathematical_reference
---

# RES-05D: Anisotropic ROF total-variation denoising with asynchronous early stopping

This is an independent early-stopping profile. It does not add a stop-mode switch to the fixed-step member. Inherit the full input/output, shape, boundary, recurrence, numeric and resource contract from [RES-05](RES-05_contract.md) and the corresponding fixed-step member [RES-05B_tv_rof_anisotropic.md](RES-05B_tv_rof_anisotropic.md). All parameters, including `y_axis`, `x_axis`, `max_iterations`, check interval, `atol`, `rtol` and algorithm controls, are explicit required inputs; factories supply no defaults.

## Recurrence and state

Use the anisotropic ROF objective and the same Chambolle–Pock recurrence, initialization, dual constraints, boundary, and stage rounding as RES-05B. This is a separate early-stop profile.

The iterative state follows the main input dtype `t`; publish the final output directly with RN_t. Fixed-step profiles do not stop early. This independent member requires explicit `max_iterations`, check interval, `atol`, and `rtol`, in addition to all RES-05A algorithm parameters.

## Stopping criterion

Use the per-plane primal–dual gap criterion `(P(u)-D(p_hat))/N <= atol + rtol*max(abs(P(u))/N, abs(D(p_hat))/N)`, with N pixels and a dual verification state that is explicitly made feasible. `atol` is in average-objective units and `rtol` is dimensionless. Apply NUM-compatible exact discrete comparisons; NaN/Inf is not convergence evidence. The check does not claim perceptual quality.

## Asynchronous lifecycle

Use [FILTER iterative contract](FILTER_iterative_contract.md) for check scheduling, consistent state ownership, dropped busy triggers, termination races, cancellation, and cleanup. Do not expose checkpoints, iteration diagnostics, or public stopping-result fields. The result is the accepted state defined by that shared contract; it is not a cached cross-call result.

## Errors and resource limits

Parameter/control-model domains remain member-specific. Ordinary numeric propagation follows the corresponding NUM operation; a non-finite value needed by a stopping check makes that check inconclusive and it does not request convergence. Invalid parameters, resource exhaustion, cancellation, upstream failures and arithmetic errors retain their own failure behavior. Charge retained check states and concurrent work to the execution budget. Do not silently change algorithm, precision, or iteration limit.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior, max-iteration termination, and the shared lifecycle contract. Runtime registration and implementation are not implied by this proposal.
