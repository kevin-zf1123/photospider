---
spec_schema_version: 1
id: NOI-03A
parent_id: NOI-03
function: correlate_kernel
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.correlate_kernel_v1_strict
  - noise.correlate_kernel_v1_accelerated_apple_silicon
  - noise.correlate_kernel_v1_accelerated_x86_64
---

# NOI-03A: correlate kernel

Inherit [NOI-03](NOI-03_correlated_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
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
validate the full kernel and shared controls. Full shared-control/topology/resource
changes conservatively invalidate dependent output; sample changes follow the actual
dependency map. Empty requests perform static preflight without payload reads or
advancing a random sequence. Compute only requested output mathematics, retaining
required shared validation.

Halo reads mapped support of Q; full kernel validation; O(C*area(Q)*Kh*Kw).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
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
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
