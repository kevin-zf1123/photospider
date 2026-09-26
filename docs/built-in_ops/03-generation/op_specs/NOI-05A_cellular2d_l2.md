---
spec_schema_version: 1
id: NOI-05A
parent_id: NOI-05
function: cellular2d_l2
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.cellular2d_l2_v1_strict
  - noise.cellular2d_l2_v1_accelerated_apple_silicon
  - noise.cellular2d_l2_v1_accelerated_x86_64
---

# NOI-05A: cellular2d l2

Inherit [NOI-05](NOI-05_cellular_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,2]; f1,f2[S...], nearest_cell[S...,2] Int64.

## Parameters and domain

dtype,seed/stream/frame; signed32 lattice; L2 metric; max_search_rings default 64.

## Mathematical specialization

Each domain7 cell gets two halfopen53 offsets, published RN64 with explicit
half-open-cell endpoint adjustment. Sort all supported sites by
(distance_squared,iy,ix). Expand Chebyshev rings until the unvisited-cell closure lower
bound is strictly greater than current F2; equality cannot discard a tie. If proof needs
out-of-range cells, fail rather than truncating the field.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries; O(count(Q)*R^2) reference; charge rings and certification.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

F1<=F2, brute-force crosscheck with exterior certificate, negative cells and distance
ties; ring exhaustion fails.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
