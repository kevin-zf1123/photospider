---
spec_schema_version: 1
id: RES-06
kind: family_contract
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-06A
- RES-06B
- RES-06C
- RES-06D
---

# RES-06: Perona–Malik anisotropic diffusion

The two conductance functions are separate profiles. Their parameters are not interchangeable with ROF TV lambda.

Inherit the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). These proposed specifications do not claim runtime implementation or registration.

Image inputs use distinct static Int64 `y_axis` and `x_axis`; all other axes are independent batch/channel planes.

| ID | Profile | Type | Numeric behavior |
| --- | --- | --- | --- |
| RES-06A | [Exponential conductance, fixed steps](RES-06A_diffusion_exponential.md) | primitive | S+B |
| RES-06B | [Reciprocal conductance, fixed steps](RES-06B_diffusion_reciprocal.md) | primitive | S+B |
| RES-06C | [Exponential conductance, asynchronous early stop](RES-06C_diffusion_exponential_early_stop.md) | primitive | S+B |
| RES-06D | [Reciprocal conductance, asynchronous early stop](RES-06D_diffusion_reciprocal_early_stop.md) | primitive | S+B |

## Shared recurrence and units

For each plane, perform explicit synchronous time steps over the finite grid with no-flux boundary. For each undirected in-domain neighbor edge `(i,j)`, compute `d=(u_j-u_i)/spacing_ij` and one baked64 conductance `c_ij`, shared in both directions. Then update simultaneously:

`u_new[i] = RN_t(u_i + dt*Σ_j c_ij*(u_j-u_i)/spacing_ij^2)`.

The state follows main input dtype `t`; conductance is RN64. This scheme can exhibit small mass drift due to per-step rounding and is not bitwise mass-conserving. The family’s explicit stability condition is `dt <= 1/(2*(1/dx^2+1/dy^2))`. Units are the actual input-value scale and diffusion time/sample-spacing units supplied by the caller; no dtype-based normalization is inferred.

Fixed-step members require `max_iterations>=0`, use that as the exact number of updates, and do not stop early. Zero steps return an input bit-copy. Early-stop members are separate profiles with `rate_tolerance` and explicit check interval and iteration bound; their rate predicates and asynchronous semantics follow [FILTER iterative contract](FILTER_iterative_contract.md). No fixed maximum image size or iteration count is implied beyond checked representation and execution budgets.

Ordinary arithmetic follows NUM, including NaN, Inf, overflow, signed zero, payload and precision behavior. Validate finite positive algorithm controls and the stated control-model/stability domain. Resource exhaustion fails explicitly and must not change the algorithm, precision or requested step limit.

Whole-plane dependency applies to each fixed-step profile. Members declare exact support and dirty behavior; a theoretical finite halo does not authorize partial-state sharing. Account for two state generations, baked edge conductances, work, checker-held state and checker workspace for early-stop members. Cancellation and owner lifetime follow common execution contracts.

## Acceptance

Independently check the one-step equation, shared conductance symmetry, constant preservation, stability rejection and each early-stop predicate. Runtime ROI, dirty, ownership, cancellation and budget claims require runtime evidence beyond reference fixtures.

## References

[S22 · scikit-image restoration](../research-sources.md#s22).
