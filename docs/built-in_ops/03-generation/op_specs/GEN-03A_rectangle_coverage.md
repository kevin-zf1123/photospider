---
spec_schema_version: 1
id: GEN-03A
parent_id: GEN-03
function: rectangle_coverage
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.rectangle_coverage_v1_strict
  - generation.rectangle_coverage_v1_accelerated_apple_silicon
  - generation.rectangle_coverage_v1_accelerated_x86_64
---

# GEN-03A: rectangle coverage

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

bounds[4]=(xmin,ymin,xmax,ymax); coverage[H,W].

## Parameters and domain

canvas, dtype default Float64; xmin<=xmax and ymin<=ymax; equality gives empty area.

## Mathematical specialization

At pixel lower corner (x,y),
RN_T(max(0,min(x+1,xmax)-max(x,xmin))*max(0,min(y+1,ymax)-max(y,ymin))). Round the full
rational product once.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(area(Q)).

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Bounds (1/4,1/4,3/4,3/4) cover 1/4 of the unit pixel; reversed bounds fail; integer
translation invariance.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
