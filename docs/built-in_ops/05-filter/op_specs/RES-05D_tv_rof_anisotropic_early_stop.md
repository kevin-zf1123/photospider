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
- restoration.tv_rof_anisotropic_early_stop
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-05D: Anisotropic ROF total-variation denoising with asynchronous early stopping

This is an independent early-stopping profile, not a stop-mode switch on the fixed-step member. It inherits the input/output, shape, boundary, recurrence and numeric contract from [RES-05](RES-05_contract.md) and the fixed-step member [RES-05B_tv_rof_anisotropic.md](RES-05B_tv_rof_anisotropic.md). Arithmetic follows the [RES native floating-point contract](RES_native_contract.md).

## Interface

Operation key: `restoration.tv_rof_anisotropic_early_stop`.

All parameters, including `y_axis`, `x_axis`, `max_iterations`, check interval, `atol`, `rtol` and algorithm controls, are explicit required inputs; factories supply no defaults.

## Mathematics

Use the anisotropic ROF objective and the same Chambolle–Pock recurrence, initialization, dual constraints, boundary, and stage rounding as RES-05B. This is a separate early-stop profile.

The iterative state follows the main input dtype `t`; publish the final output directly with RN_t. Fixed-step profiles do not stop early. This independent member requires explicit `max_iterations`, check interval, `atol`, and `rtol`, in addition to all RES-05A algorithm parameters.

### Stopping criterion

Use the per-plane primal–dual gap criterion `(P(u)-D(p_hat))/N <= atol + rtol*max(abs(P(u))/N, abs(D(p_hat))/N)`, with N pixels and a dual verification state that is explicitly made feasible. `atol` is in average-objective units and `rtol` is dimensionless. Apply comparisons of the computed native values; NaN/Inf is not convergence evidence. The check does not claim perceptual quality.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior and max-iteration termination.
