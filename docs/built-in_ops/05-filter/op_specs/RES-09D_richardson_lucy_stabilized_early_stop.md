---
spec_schema_version: 1
id: RES-09D
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-09
function: richardson_lucy_stabilized_early_stop
proposed_operation_keys:
- restoration.richardson_lucy_stabilized_early_stop_strict
numeric_reference: S; asynchronous stopping profile
oracle_scope: mathematical_reference
---

# RES-09D: Stabilized Richardson–Lucy restoration with asynchronous early stopping

This is an independent early-stopping profile. It does not add a stop-mode switch to the fixed-step member. Inherit the full input/output, shape, boundary, recurrence, numeric and resource contract from [RES-09](RES-09_contract.md) and the corresponding fixed-step member [RES-09B_richardson_lucy_stabilized.md](RES-09B_richardson_lucy_stabilized.md). All controls, including `y_axis`, `x_axis`, `max_iterations`, check interval, `atol`, `rtol`, boundary, `anchor_y`, `anchor_x` and broadcast axes are mandatory. Constructors have no defaults. RES-09D additionally requires finite `epsilon>0` in observation units.

## Recurrence and state

Use RES-09B’s explicit epsilon-stabilized recurrence and true adjoint/sensitivity. Epsilon is required, strictly positive and measured in observation units.

The estimate state follows main-input dtype `t`, and output is directly RN_t. Fixed-step members never stop early; this is an independent member with its own explicit `max_iterations` bound and stop controls.

## Stopping criterion

Let exact `s=A^T*m` and `J={i:s_i>0}`. On the coherent adjacent states, stop when `max_(i in J) abs(x_k[i]-x_(k-1)[i]) <= atol + rtol*max(max_(i in J) abs(x_(k-1)[i]),max_(i in J) abs(x_k[i]))`. Both tolerances are mandatory; `atol` uses estimate-image units and `rtol` is dimensionless. Compare iteration states, not values after output conversion. `s=0` copy positions do not enter the reductions. Empty J is reported as having no observed support, not as convergence. This is a state-change criterion only.

## Asynchronous lifecycle

Use the shared [FILTER iterative contract](FILTER_iterative_contract.md) for coherent adjacent states, trigger scheduling, dropped busy checks, termination races, cancellation, resource admission and cleanup. Do not expose checkpoints, iteration diagnostics, or public stopping-result fields. The result is the accepted state defined by that shared contract; it is not a cached cross-call result.

## Errors and resource limits

Parameter/control-model domains remain member-specific. Ordinary numeric propagation follows the corresponding NUM operation; a non-finite value needed by a stopping check makes that check inconclusive and it does not request convergence. Invalid parameters, resource exhaustion, cancellation, upstream failures and arithmetic errors retain their own failure behavior. Charge retained check states and concurrent work to the execution budget. Do not silently change algorithm, precision, or iteration limit.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior, max-iteration termination, and the shared lifecycle contract. Runtime registration and implementation are not implied by this proposal.
