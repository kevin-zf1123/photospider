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
- restoration.diffusion_reciprocal_early_stop
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-06D: Reciprocal edge-preserving diffusion with asynchronous early stopping

This is an independent early-stopping profile, not a stop-mode switch on the fixed-step member. It inherits the input/output, shape, boundary, recurrence and numeric contract from [RES-06](RES-06_contract.md) and the fixed-step member [RES-06B_diffusion_reciprocal.md](RES-06B_diffusion_reciprocal.md). Arithmetic follows the [RES native floating-point contract](RES_native_contract.md).

## Interface

Operation key: `restoration.diffusion_reciprocal_early_stop`.

Every algorithm control, `y_axis`, `x_axis`, `max_iterations`, `rate_tolerance`, and check interval is mandatory; constructors have no defaults.

## Mathematics

Use RES-06B’s synchronous explicit diffusion recurrence, no-flux boundary and reciprocal conductance `c_ij=1/(1+(d_ij/kappa)^2)` in the input dtype; preserve the ordinary state in the main input dtype.

The recurrence state follows main input dtype `t`, and the final result is rounded directly to `t` with RN_t. Fixed-step profiles never stop early; this member has a distinct explicit `max_iterations` bound.

### Stopping criterion

At a check compute `F_i(u)=sum_j c_ij*(u_j-u_i)/spacing_ij^2` from the current complete state and native-precision conductances evaluated from that same state. Stop when `max_i abs(F_i(u)) <= rate_tolerance`. `rate_tolerance` is mandatory, finite, nonnegative, and measured in value-units per diffusion-time. Evaluate the defined rate directly, without using a rounded state delta divided by `dt`. The condition is a model-rate criterion and does not promise best denoising quality.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior and max-iteration termination.
