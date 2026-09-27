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

Main input dtype t controls TV u,p,ubar, diffusion state, RL v,r,t,x stages, and
blind image/residual/gradient stages. Float32 uses RN32 and Float64 uses RN64.
Explicit baked64 conductance/kernel coefficients retain RN64. Blind PSF state uses
exact rational simplex projection and exact support/nonnegativity/sum-one constraints;
charge numerator/denominator limb growth. Its public PSF is directly RN_t(exact state),
with no intermediate RN64 for Float32 and no output renormalization. Rounded PSF
sum is not promised to equal exactly one.

There is no public checkpoint format. Ordinary outputs supplied as a new initial
state do not imply continuation of an uninterrupted solver, especially for TV dual
states or the rational PSF. Execution diagnostics carry returned iteration and reason;
there are no additional result ports. Diagnostic encoding uses the runtime's existing
facility when implemented. Reason names below are logical contract labels.

## Stopping predicates

All tolerances are explicit, finite and nonnegative. Checks use exact expressions
on the declared stored states, with certified transcendental evaluation where needed.
NUM accelerated final-output bounds do not weaken a discrete stopping comparison.
Required NaN/Inf that prevents meaningful evaluation makes a check not passing;
Inf<=Inf is never convergence evidence. Do not confuse temporary host overflow with
a nonfinite mathematical value. Parameter/resource/upstream failures remain failures.
All independent planes must pass in the same coherent round.

### ROF TV

For N pixels, forward-difference K and family lambda, define
P(u)=0.5*||u-f||² + lambda*sum(phi(Ku)). Given a feasible dual q,
D(q)=<f,K^T q>-0.5*||K^T q||². The exact gap is nonnegative and the test is

```
(P(u)-D(q))/N <= atol + rtol*max(abs(P(u))/N,abs(D(q))/N)
```

atol has average-objective units and rtol is dimensionless. Construct q from the
checked round's stored p without changing p: for isotropic TV use
q_i=p_i/max(1,||p_i||_2/lambda); for anisotropic TV clip each component to
[-lambda,lambda]. Invalid forward-boundary dual components remain zero. This check
projection is an exact expression, not rounded back to t, so feasibility does not
rely on rounded solver state. lambda=0 is the member's identity branch; no division
by lambda is evaluated there. Directed bounds must certify the actual inequality,
including exact equality handling or an explicit resource failure. This certifies
objective progress on finite data, not perceptual quality.

### Diffusion

Using the checked current state and its profile's baked64 conductance,
F_i(u)=sum_j c_ij*(u_j-u_i)/spacing_ij². Test max_i(abs(F_i))<=rate_tolerance.
The tolerance has value/diffusion-time units. Do not multiply the predicate by dt
or replace F with rounded state differences divided by dt. Conductance underflow
to zero is part of the profile and can affect stationary states. This is a rate
criterion, not a best-denoising score.

### Richardson–Lucy

Let s=A^T*m be exact and J={i:s_i>0}. On the coherent actual adjacent states:

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
The check uses exact objective-gradient expressions on the stored pair, not rounded
solver update intermediates. With eta_x=step_image and eta_k=step_psf:

```
G_x=(x-project_nonnegative(x-eta_x*grad_x E(x,k)))/eta_x
G_k=(k-project_simplex_support(k-eta_k*grad_k E(x,k)))/eta_k
max_i abs(G_x[i]) <= epsilon_x
max_(j in support) abs(G_k[j]) <= epsilon_k
```

Both inequalities must hold. Each threshold has its own residual units; there is no
combined norm, RMS threshold or independent check-step parameter. The exact simplex
projection is the family's support-constrained projection. The check leaves state
unchanged and establishes only constrained first-order stationarity, not uniqueness,
global optimality or perceptual quality.

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
state pair, and blind PSF pins its same-round image/rational-PSF pair. A single
atomic admission/publication protocol prevents overlapping checks or mixed rounds.

The checker reads those existing memory blocks without copying payload or holding
a data mutex during traversal. Computation continues using other admitted buffers.
Brief publication/ownership synchronization is allowed. An atomic pointer alone
does not permit overwriting ordinary floats while a checker reads them. Owners
remain pinned until checking completes or safely cancels.

Reserve bounded capacity for a check-held coherent state set, computation buffers
and checker workspace before execution. Do not assume a universal triple-buffer
count; each solver's required live states determine it. Rational limb growth remains
dynamically budgeted. On shortage, report resource failure, not synchronous checking,
payload copying, overwrite, reduced precision or unaccounted allocation.

## Termination and cache identity

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
profiles. Fixed-step profile determinism/cache behavior is unchanged.

Acceptance must cover dropped triggers, delayed starts, retained state immutability,
coherent pairs, both termination race winners, max=0, no final check, nonfinite
non-passing checks, cancellation, failures and budget cleanup. Mathematical predicate
fixtures alone do not verify the concurrent runtime protocol.

## Exceptional PSF projection state

Exact-rational simplex feasibility is guaranteed for finite projection arguments.
A NaN/Inf argument has no finite Euclidean simplex projection. Apply NUM invalid
arithmetic propagation instead of converting it to a rational or reporting floating
overflow as failure: represent this exceptional state by a NaN tag and publish the
same quiet NaN at every selected support tap, with +0 outside support. Choose the
first NaN argument in original support tap order, preserving its NUM sign/payload;
if there is only infinity, generate the fixed positive quiet NaN. This tag is not
a feasible simplex certificate and does not claim sum-one. Later computations
consume those exceptional components under NUM; the exceptional PSF remains tagged.
No early-stop check can pass on that state. Fixed-step execution still completes
its declared count, and early-stop execution can end at max_iterations. Required
initial finite PSF/domain validation, resource errors and cancellation remain errors.
The finite constrained state still uses exact rationals without intermediate Float64
rounding. `special.exceptional_simplex` covers the projection branch, not a complete
concurrent solver implementation.
