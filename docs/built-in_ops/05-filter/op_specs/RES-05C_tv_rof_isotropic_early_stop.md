---
spec_schema_version: 1
id: RES-05C
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-05
function: tv_rof_isotropic_early_stop
proposed_operation_keys:
- restoration.tv_rof_isotropic_early_stop
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-05C: Isotropic ROF total-variation denoising with asynchronous early stopping

This is an independent early-stopping profile, not a stop-mode switch on the fixed-step member. It inherits the input/output, shape, boundary, recurrence and numeric contract from [RES-05](RES-05_contract.md) and the fixed-step member [RES-05A_tv_rof_isotropic.md](RES-05A_tv_rof_isotropic.md). Arithmetic follows the [RES native floating-point contract](RES_native_contract.md).

## Interface

Operation key: `restoration.tv_rof_isotropic_early_stop`.

All parameters, including `y_axis`, `x_axis`, `max_iterations`, check interval, `atol`, `rtol` and algorithm controls, are explicit required inputs; factories supply no defaults.

## Mathematics

For each independent plane, use the isotropic ROF objective and the same Chambolle–Pock recurrence, initialization, primal/dual constraints, boundary, and stage rounding as RES-05A. The profile differs only by its explicit stopping controls and asynchronous lifecycle.

The iterative state follows the main input dtype `t`; publish the final output directly with RN_t. Fixed-step profiles do not stop early. This independent member requires explicit `max_iterations`, check interval, `atol`, and `rtol`, in addition to all RES-05A algorithm parameters.

### Stopping criterion

Use the per-plane primal–dual gap predicate and native feasible-dual construction specified in [FILTER iterative contract](FILTER_iterative_contract.md). Required `atol` is in average-objective units and `rtol` is dimensionless. Both are finite and nonnegative. The check uses the same coherent round’s primal and dual state, does not alter solver state, and uses comparisons of the computed native values; NaN/Inf cannot establish convergence.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior and max-iteration termination.
