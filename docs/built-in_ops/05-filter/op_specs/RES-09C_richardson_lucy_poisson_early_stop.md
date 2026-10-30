---
spec_schema_version: 1
id: RES-09C
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-09
function: richardson_lucy_poisson_early_stop
proposed_operation_keys:
- restoration.richardson_lucy_poisson_early_stop
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-09C: Poisson Richardson–Lucy restoration with asynchronous early stopping

This is an independent early-stopping profile, not a stop-mode switch on the fixed-step member. It inherits the input/output, shape, boundary, recurrence and numeric contract from [RES-09](RES-09_contract.md) and the fixed-step member [RES-09A_richardson_lucy_poisson.md](RES-09A_richardson_lucy_poisson.md). Arithmetic follows the [RES native floating-point contract](RES_native_contract.md).

## Interface

Operation key: `restoration.richardson_lucy_poisson_early_stop`.

All controls, including `y_axis`, `x_axis`, `max_iterations`, check interval, `atol`, `rtol`, boundary, `anchor_y`, `anchor_x` and broadcast axes are mandatory. Constructors have no defaults. RES-09C fixes epsilon at zero and has no hidden floor parameter.

## Mathematics

Use RES-09A’s unregularized Richardson–Lucy recurrence, finite forward operator, its true transpose, native sensitivity `s=A^T m`, and the same explicit domain restrictions and per-step rounding.

The estimate state follows main-input dtype `t`, and output is directly RN_t. Fixed-step members never stop early; this is an independent member with its own explicit `max_iterations` bound and stop controls.

### Stopping criterion

Let s=A^T*m be native and J be the structurally observed support defined in RES-09. On the coherent adjacent states, stop when `max_(i in J) abs(x_k[i]-x_(k-1)[i]) <= atol + rtol*max(max_(i in J) abs(x_(k-1)[i]),max_(i in J) abs(x_k[i]))`. `atol` is in estimate-image units and `rtol` is dimensionless; both are explicit parameters. Compare the member’s iteration states, not output-dtype-converted values. Structurally unobserved copy positions are excluded; a rounded zero on observed support does not change J. If J is empty, report no observed support as defined by the family contract; do not call it convergence. This criterion means state change is small, not that the solution is optimal.

## Acceptance

Verify the fixed recurrence independently, then verify the criterion on hand-computed states, a case that meets it, a case that does not, non-finite check behavior and max-iteration termination.
