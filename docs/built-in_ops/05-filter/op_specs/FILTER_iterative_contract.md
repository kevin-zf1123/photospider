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
states or the full alternating solver state. Implementations track returned iteration and reason in invocation-owned state. Fixed invocation ABI 8 has no report-submission callback; it does not automatically export these logical outcome labels as execution diagnostics. An explicit SDK reporting interface would require its own specification before claiming that capability; no additional result port or kernel ABI change is implied.

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
[-lambda,lambda]. Invalid forward-boundary dual components remain zero. This check projection uses native precision and leaves solver state unchanged. lambda=0 is the identity branch and does not divide by lambda. Validate gap evaluation and feasibility error against a higher-precision reference. Passing the native inequality indicates numerical objective progress, not an exact feasibility certificate or perceptual quality.

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

## Fixed execution boundary

Fixed-step recurrences can use existing CFG loops, explicit edge arguments and declared intermediate tasks. The asynchronous profiles below retain their distinct timing semantics and remain unimplemented. Their concurrent solver/checker must be owned by one bounded operation invocation (or a separately specified host workflow); it cannot append tasks, rediscover dependencies or expose mutable state through a published Result. All worker state and scratch must fit declared bounds, and accepted work must be joined or drained before reporting completion. A serial CFG loop does not establish the asynchronous profile's acceptance.

Solver/checker borrows of invocation views, scratch, cancellation state and Run
services must be released on every exit, including partial startup failure,
before reporting invocation completion. Closing a Context does not extend those
borrows. An independently retained immutable Result may survive the Context
under the shared owner contract; mutable checker state must still settle within
its owning invocation.

## Trigger and ownership protocol

Each algorithm uses a fixed positive interval K chosen through implementation
measurement. This specification does not prescribe K values. Trigger after completed
rounds K,2K,...; do not check round 0. max_iterations=0 returns the member's zero-step
result without a checker. Identity branches report identity, not converged. There
is no forced final check.

At most one checker is active per solve. Busy triggers are dropped, with no queue
or pending flag. When an accepted checker starts, acquire a coherent immutable
reference set for the latest complete round then available. That may be newer than
the trigger's round. TV pins corresponding primal/dual state, RL pins its adjacent
state pair, and blind PSF pins its same-round native image/PSF pair. A single
atomic admission/publication protocol prevents overlapping checks or mixed rounds.

The checker reads those existing memory blocks without copying payload or holding
a data mutex during traversal. Computation continues using other admitted buffers.
Brief publication/ownership synchronization is allowed. An atomic pointer alone
does not permit overwriting ordinary floats while a checker reads them. Owners
remain pinned until checking completes or safely cancels.

Reserve bounded capacity for a check-held coherent state set, computation buffers
and checker workspace before execution. Do not assume a universal triple-buffer
count; each solver's required live states determine it. Native state and projection workspace are budgeted. On shortage, report resource failure, not synchronous checking,
payload copying, overwrite, reduced precision or unaccounted allocation.

## Termination and invocation identity

A passing checker atomically proposes (converged, checked_round, result_owners).
Return that checked state after the member's final output conversion; discard newer
speculative states. Signal cooperative stop and poll safe computation checkpoints.
The criterion is evidence about internal state, not necessarily the rounded public
PSF/output. No asynchronous thread may retain unsafe access after call completion.

When max_iterations is reached, atomically propose (max_iterations, final_round,
final_owners), cancel the in-flight checker and perform safe cleanup without waiting
for the entire predicate or adding a final check. The first successfully committed
normal termination record wins and cannot be overwritten. Reason, round and owners
are one coherent record. Failures are never converted to normal success. Finish
necessary joins, ownership releases and error handling before returning.

Scheduling can change checked rounds and returned bits across independent calls.
Do not promise the earliest passing round or repeatability of the chosen round.
The recurrence and predicate math remain fixed. Sharing within one invocation is
legal; cross-independent-invocation result caching is forbidden for early-stop
profiles. Fixed-step profiles retain their deterministic recurrence; the frozen executor has no cross-Run result cache for either profile.

Acceptance must cover dropped triggers, delayed starts, retained state immutability,
coherent pairs, both termination race winners, max=0, no final check, nonfinite
non-passing checks, cancellation, failures and budget cleanup. Mathematical predicate
fixtures alone do not verify the concurrent runtime protocol.

## Exceptional PSF projection state

Finite simplex projection follows RES-10's native sorted-threshold algorithm. A non-finite projection argument produces NaN at selected support taps and +0 outside support, with no payload/sign guarantee. This is not a feasible simplex certificate and cannot pass an early-stop check. Fixed-step execution retains its declared count. Caller model premises, resource failure, cancellation and ownership are unchanged. Existing rational exceptional-value helpers are mathematical diagnostics, not native bitwise goldens.
