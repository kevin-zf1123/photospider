---
spec_schema_version: 1
id: NOI-03A
parent_id: NOI-03
function: correlate_kernel
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.correlate_kernel_v1_strict
  - noise.correlate_kernel_v1_accelerated_apple_silicon
  - noise.correlate_kernel_v1_accelerated_x86_64
---

# NOI-03A: correlate kernel

Inherit [NOI-03](NOI-03_correlated_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

source[H,W,C], kernel[Kh,Kw], finite Float32/64; same-shape values.

## Parameters and domain

Positive odd kernel extents, default each <=255; boundary=zero/clamp/wrap/reflect101
required; normalization=none/sum/l2, default none; output dtype explicit.

## Mathematical specialization

Correlation, not flipped convolution: RN_T(sum w[dy,dx]*sample(y+dy-rh,x+dx-rw)/den).
den=1,sum(w),sqrt(sum(w^2)) respectively; nontrivial denominator must be nonzero.
reflect101 period=2(N-1), N=1 maps to zero index. Repeated boundary indices are the same
variable.

## Dependencies, resources and failures

Execution rule: **Halo**. Read only the boundary-mapped source halo required by Q;
read the kernel and shared controls required by the mathematics, assuming valid payloads. Full shared-control/topology/resource
changes conservatively invalidate dependent output; sample changes follow the actual
dependency map. Empty requests perform static preflight without payload reads or
advancing a random sequence. Compute only requested output mathematics, retaining
the declared mathematical dependencies and static structure checks.

Halo reads mapped support of Q; valid kernel coefficients and normalization denominators are caller premises; O(C*area(Q)*Kh*Kw).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Asymmetric kernel direction, 1x1 identity, singleton reflect, sum-normalized constant
field and merged boundary weights.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
