---
spec_schema_version: 1
id: GEN-05G
parent_id: GEN-05
function: two_circle_radial_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.two_circle_radial_coordinate_v1_strict
  - generation.two_circle_radial_coordinate_v1_accelerated_apple_silicon
  - generation.two_circle_radial_coordinate_v1_accelerated_x86_64
---

# GEN-05G: two circle radial coordinate

Inherit [GEN-05](GEN-05_gradient_coordinates_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

c0[2],r0[1],c1[2],r1[1]; t[H,W] and UInt8 valid[H,W].

## Parameters and domain

canvas/dtype; nonnegative finite radii, distinct circles; no_solution=valid_zero default
or reject.

## Mathematical specialization

q=p-c0,d=c1-c0,s=r1-r0. Solve A*t^2+B*t+C=0 with A=dot(d,d)-s^2, B=-2*(dot(q,d)+r0*s),
C=dot(q,q)-r0^2. Keep real roots with r0+t*s>=0; take the largest before spread. Linear
cases use exact zero classification. If A=B=C=0, s<0 has maximum -r0/s; otherwise no
finite maximum. No returnable coordinate gives (+0,0) or failure; success valid=1.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)); certified roots and exact branch/radius decisions; failures from budgets are
not invalid coordinates.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Concentric 0->2 at distance 1 gives 0.5; equal-radius translated circles select larger
root; no roots, tangency, linear and identity-equation cases; identical circles fail.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
