---
spec_schema_version: 1
id: RES-09A
kind: primitive
category: 05-filter
status: Proposed
implementation_status: not_implemented
parent_id: RES-09
function: richardson_lucy_poisson
proposed_operation_keys:
- restoration.richardson_lucy_poisson_strict
- restoration.richardson_lucy_poisson_accelerated_apple_silicon
- restoration.richardson_lucy_poisson_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
---

# RES-09A: Richardson–Lucy, Poisson fixed-step profile

Inherit the [RES-09 family contract](RES-09_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Ports

`observation, psf, initial, background, mask` → independently requestable `estimate, unobserved`. `unobserved` is UInt8 and has the observation’s spatial shape. Inputs share H/W except explicitly broadcast scalar controls. This is a raw numerical operation; it does not infer color or alpha from channel count.

## Required parameters and model domain

Require explicit `max_iterations>=0`, `y_axis`, `x_axis`, kernel `anchor_y`, `anchor_x`, `boundary∈{constant0,wrap}`, `broadcast_axes`, output dtype and output requests. This member’s epsilon is zero (fixed). Factories have no defaults, including for boundary, anchor, broadcast axes and output requests. PSF taps are nonnegative with positive exact sum. Mask values lie in `[0,1]`; finite Poisson-model observation, estimate and background values must be nonnegative. Negative values outside the model are `InvalidDomain`. For NUM-permitted NaN/Inf values, use ordinary NUM arithmetic propagation; do not blanket-reject them as finite-only data. Mask-zero taps set `r=+0` without reading observation.

## Recurrence

Normalize the PSF by exact rational division to define finite operator `A`; compute its actual transpose `Aᵀ` by scatter over the finite domains and exact sensitivity `s=Aᵀm`. Do not infer the transpose by merely flipping the PSF. Initialize `x` from `initial` converted to main-input dtype `t`. Each full synchronous Jacobi step is:

`v=RN_t(A*x+background)`;
`r_i=+0` at `m_i=0` without reading `y_i`, otherwise `RN_t(m_i*y_i/(v_i+epsilon))`;
`q=RN_t(Aᵀ*r)`;
`x_new_i = RN_t(x_i*q_i/s_i)` if `s_i>0`, else bit-copy `x_i`.

For RES-09A, epsilon is exactly zero: if the exact denominator is zero, `y=0` gives `r=0`, while `y>0` is `InvalidDomain`. RES-09B has strictly positive epsilon and therefore does not take that zero-denominator branch. Keep exact sensitivity through division; never round it to RN64. No flux renormalization is imposed per step. Do not clip output or replace negative values by zero. Zero steps produce an estimate bit-copy. No data-dependent early stop is permitted for these members.

## Demand and resource behavior

Estimate demand reads Whole plane and full PSF. `unobserved`-only demand does not read observation/background/initial payload. An estimate-only zero-step request copies initial but still validates PSF/mask descriptors needed for the other requested output or preflight. Unrequested payload is not read. The output descriptor depends on input descriptors and static parameters.

The direct algorithm costs `O(max_iterations*P*C*A)` and retains the finite `A` and `Aᵀ` plans plus exact sensitivity and state buffers. Any FFT path must reproduce the finite operator and rounding stages, not use circular convolution. Budget work and memory; on insufficiency fail without spill, hidden approximation or partial publication. Cancellation and owner lifetime follow shared contracts.

## Oracle and acceptance

The oracle function `restoration.rl(observation, psf, initial, background, mask, *, iterations=1, epsilon=0, anchor=(0,0), boundary="constant", dtype="float64", request_estimate=True, request_unobserved=True)` and `restoration.forward_adjoint(...)` are in [restoration.py](../../../../oracle/ops/filter/oracles/restoration.py). Map the proposed `max_iterations` parameter to oracle argument `iterations`, and map the oracle tuple `anchor=(y,x)` from public `anchor_y` and `anchor_x`. Reference fixtures include: `rl_delta_one_step` (ExactRationalStaged), `rl_masked_nonfinite_observation` (ExactRationalStaged), `rl_true_adjoint_dot` (ExactRational), and `rl_zero_prediction_positive_observation` (ContractFixture). These fixtures cover the listed reference cases only; they do not prove runtime support or the full port/preflight contract. Also verify zero-step identity, mask-zero no-read behavior, exact adjoint, output-demand independence, boundary sensitivity, parameter errors, resource failure and cancellation. See [oracle README](../../../../oracle/ops/filter/README.md).

The strict and CPU accelerated keys are proposed and not registered. Accelerated variants must satisfy the NUM final FP32-scaled four-ULP bound, preserve exact discrete decisions and use strict fallback when required. No runtime or performance evidence is claimed.

### Exceptional ratio branch

After required control/model validation, a zero mask excludes observation arithmetic.
For a positive mask, an observation NaN propagates with NUM operand priority even
when the prediction denominator is zero; it is neither the y=0 branch nor the y>0
domain-error branch. A prediction NaN also propagates when no earlier numerator NaN
has priority. Valid finite zero denominators retain the explicit zero/positive
observation rules above. Scalar fixtures are provided by `special.rl_ratio`;
complete exceptional solver trajectories remain outside the finite-rational oracle.
