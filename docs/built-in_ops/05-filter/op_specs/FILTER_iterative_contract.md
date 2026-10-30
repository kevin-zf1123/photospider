---
spec_schema_version: 1
id: FILTER-iteration
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# Iterative profiles and asynchronous stopping

## Profile separation and state precision

RES-05A/B, RES-06A/B, RES-09A/B and RES-10A are fixed-step profiles.
RES-05C/D, RES-06C/D, RES-09C/D and RES-10B are distinct asynchronous early-stop
members using the corresponding fixed-step recurrence. No stop_mode parameter or
implicit early exit changes a fixed-step profile. Every iteration is a completed
Jacobi/staged update defined by its family, not an arbitrary kernel launch.

All profiles in this document use [RES native floating-point arithmetic](RES_native_contract.md). Main input dtype t controls TV primal/dual/extrapolated state, diffusion conductance/state, RL forward/ratio/adjoint/sensitivity/estimate stages, and blind image/residual/gradient/PSF state. Native rounding occurs within each stage. Blind simplex projection retains the sorted-threshold algorithm, exact support zeros and nonnegativity; unit sum is subject to measured native roundoff. No exact-rational state or limb-growth budget is required. Do not renormalize the published PSF.

There is no public checkpoint format. Ordinary outputs supplied as a new initial
state do not imply continuation of an uninterrupted solver, especially for TV dual
states or the full alternating solver state.

## Stopping predicates

All tolerances are explicit, finite and nonnegative. Checks evaluate the declared expressions in native precision on coherent stored states. Compare the computed values with the stated inequalities. These are numerical stopping criteria, not exact-expression certificates. A required non-finite value makes a check not passing; Inf<=Inf is never convergence evidence. All independent planes must pass in the same coherent round. Parameter/resource/upstream failures remain failures.

### ROF TV

For N pixels, forward-difference K and family lambda, define
P(u)=0.5*||u-f||² + lambda*sum(phi(Ku)). Given a feasible dual q,
D(q)=<f,K^T q>-0.5*||K^T q||². The mathematical gap is nonnegative; native evaluation can incur roundoff. The test is

```
(P(u)-D(q))/N <= atol + rtol*max(abs(P(u))/N,abs(D(q))/N)
```

atol has average-objective units and rtol is dimensionless. Construct q from the
checked round's stored p without changing p: for isotropic TV use
q_i=p_i/max(1,||p_i||_2/lambda); for anisotropic TV clip each component to
[-lambda,lambda]. Invalid forward-boundary dual components remain zero. This check projection uses native precision and leaves solver state unchanged. lambda=0 is the identity branch and does not divide by lambda. Passing the native inequality indicates numerical objective progress, not an exact feasibility certificate or perceptual quality.

### Diffusion

Using the checked current state and its profile's native conductance,
F_i(u)=sum_j c_ij*(u_j-u_i)/spacing_ij². Test max_i(abs(F_i))<=rate_tolerance.
The tolerance has value/diffusion-time units. Do not multiply the predicate by dt
or replace F with rounded state differences divided by dt. Conductance underflow
to zero is part of the profile and can affect stationary states. This is a rate
criterion, not a best-denoising score.

### Richardson–Lucy

Let s=A^T*m use native coefficients and arithmetic; J is the structurally observed support defined by RES-09, independent of a rounded zero sensitivity. On the coherent actual adjacent states:

```
max_(i in J) abs(x_k[i]-x_(k-1)[i])
 <= atol + rtol*max(max_(i in J) abs(x_(k-1)[i]),max_(i in J) abs(x_k[i]))
```

atol has image units and rtol is dimensionless. Copy-only unobserved locations do
not enter either reduction. J empty follows the zero-information identity branch
with no_observed_support diagnostic, never converged. This is state stability,
including possible multiplicative zero locking, not optimality. The epsilon and
no-epsilon profiles check their own recurrence states, before final output conversion.

### Blind PSF

At one completed image/PSF pair (x,k), recompute both gradients of the joint objective
E from that same pair. Do not reuse gradients from different alternating stages.
The check recomputes native objective gradients on the stored pair; it must not reuse gradients from a different alternating stage. With eta_x=step_image and eta_k=step_psf:

```
G_x=(x-project_nonnegative(x-eta_x*grad_x E(x,k)))/eta_x
G_k=(k-project_simplex_support(k-eta_k*grad_k E(x,k)))/eta_k
max_i abs(G_x[i]) <= epsilon_x
max_(j in support) abs(G_k[j]) <= epsilon_k
```

Both inequalities must hold. Each threshold has its own residual units; there is no
combined norm, RMS threshold or independent check-step parameter. The native simplex
projection is the family's support-constrained projection. The check leaves state
unchanged and tests numerical constrained first-order stationarity, not uniqueness,
global optimality or perceptual quality.

## Early-stop semantics

The asynchronous profiles trigger a stopping check every fixed positive number of completed rounds K (K is not specified here), after rounds K, 2K, ...; round 0 is not checked. At most one check is active per solve. A trigger that occurs while a check is active is discarded, without queuing or a pending check; it is not replayed after that check completes. max_iterations=0 returns the member's zero-step result without a check. Identity branches report identity, not converged. There is no forced final check. A check evaluates the latest coherent state set available when it starts: TV uses corresponding primal/dual state, RL uses its adjacent state pair, and blind PSF uses its same-round image/PSF pair. Mixed rounds are never combined.

A passing check proposes its checked state for normal termination. If it wins, that state is returned after the member's final output conversion and newer speculative states are discarded. The criterion is evidence about internal state, not necessarily the rounded public PSF/output. Reaching max_iterations proposes the final-round state for normal termination without a final check or waiting for an active predicate to finish. Reason, round and returned state form one coherent record, and the first normal termination record wins; a later check cannot replace a committed max_iterations result. Failures are never converted to normal success.

Which round is returned can differ between independent calls; the earliest passing round and its repeatability are not promised. The recurrence and predicate math are fixed. Fixed-step profiles are deterministic.

There is no public checkpoint format. Ordinary outputs supplied as a new initial state do not imply continuation of an uninterrupted solver, especially for TV dual states or the full alternating solver state. The returned iteration count and reason are logical outcome labels, not additional result ports.

## Exceptional PSF projection state

Finite simplex projection follows RES-10's native sorted-threshold algorithm. A non-finite projection argument produces NaN at selected support taps and +0 outside support, with no payload/sign guarantee. This is not a feasible simplex certificate and cannot pass an early-stop check. Fixed-step execution retains its declared count.
