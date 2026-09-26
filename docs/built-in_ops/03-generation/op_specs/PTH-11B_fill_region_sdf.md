---
spec_schema_version: 1
id: PTH-11B
parent_id: PTH-11
function: fill_region_sdf
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.fill_region_sdf_v1_strict
  - path.fill_region_sdf_v1_accelerated_apple_silicon
  - path.fill_region_sdf_v1_accelerated_x86_64
---

# PTH-11B: fill region sdf

Inherit [PTH-11](PTH-11_distance_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Polyline PathSet; distance[H,W]; optional closest/provenance requires formal associated
schema.

## Parameters and domain

canvas/dtype/fill_rule; optional positive finite max_distance; source curves explicitly
flattened.

## Mathematical specialization

Use regularized filled region and its exposed boundary. Distance to that boundary with
inside negative, boundary +0; remove internal shared/overlap edges. Optional signed
magnitude clipping. Empty region returns +max only when supplied, otherwise NoSolution.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries with exact global boundary arrangement; reference O(area(Q)*boundary).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Rectangle interior, adjacent union shared edge not zero, holes, repeated evenodd
contours empty and signed clipping.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
