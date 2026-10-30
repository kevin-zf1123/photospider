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
- restoration.richardson_lucy_poisson
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-09A: Richardson–Lucy, Poisson fixed-step profile

Inherit the [RES-09 family contract](RES-09_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Interface

Operation key: `restoration.richardson_lucy_poisson`.

### Ports

`observation, psf, initial, background, mask` → independently requestable `estimate, unobserved`. `unobserved` is UInt8 and has the observation’s spatial shape. Inputs share H/W except explicitly broadcast scalar controls. This is a raw numerical operation; it does not infer color or alpha from channel count.

### Parameters

Require explicit `max_iterations>=0`, `y_axis`, `x_axis`, kernel `anchor_y`, `anchor_x`, `boundary∈{constant0,wrap}`, `broadcast_axes`, output dtype and output requests. This member’s epsilon is zero (fixed). Factories have no defaults, including for boundary, anchor, broadcast axes and output requests. PSF taps are nonnegative with positive native sum. Mask values lie in `[0,1]`; finite Poisson-model observation, estimate and background values must be nonnegative. The stated nonnegative numerical/model domains are unchecked caller premises. For NaN/Inf values, use native arithmetic propagation; do not blanket-reject them as finite-only data. Mask-zero taps set `r=+0` without reading observation.

## Mathematics

Normalize the PSF by native input-dtype division to define finite operator `A`; compute its actual transpose `Aᵀ` by scatter over the finite domains and native sensitivity `s=Aᵀm`. Do not infer the transpose by merely flipping the PSF. Initialize `x` from `initial` converted to main-input dtype `t`. Each full synchronous Jacobi step is:

`v=RN_t(A*x+background)`;
`r_i=+0` at `m_i=0` without reading `y_i`, otherwise `RN_t(m_i*y_i/(v_i+epsilon))`;
`q=RN_t(Aᵀ*r)`;
`x_new_i = RN_t(x_i*q_i/s_i)` if `s_i>0`, else bit-copy `x_i`.

For RES-09A, epsilon is exactly zero: if the computed native denominator is zero, `y=0` gives `r=0`, while `y>0` is `InvalidDomain`. RES-09B has strictly positive epsilon and therefore does not take that zero-denominator branch. Use the same stored native sensitivity in each division; structural unobserved support follows the family contract independently of rounded sensitivity zeros. No flux renormalization is imposed per step. Do not clip output or replace negative values by zero. Zero steps produce an estimate bit-copy. No data-dependent early stop is permitted for these members.

### Exceptional ratio branch

Under the caller-supplied control/model premises, a zero mask excludes observation arithmetic.
For a positive mask, a NaN observation or prediction propagates to a NaN ratio; it is neither the finite y=0 branch nor the finite y>0 no-solution branch. No NaN payload priority is promised. Valid finite zero denominators retain the explicit zero/positive observation rules above.
