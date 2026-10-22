---
spec_schema_version: 1
id: GEN-03A
parent_id: GEN-03
function: rectangle_coverage
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.rectangle_coverage_v1_strict
  - generation.rectangle_coverage_v1_accelerated_apple_silicon
  - generation.rectangle_coverage_v1_accelerated_x86_64
---

# GEN-03A: rectangle coverage

Inherit [GEN-03](GEN-03_basic_shapes_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

bounds[4]=(xmin,ymin,xmax,ymax); coverage[H,W].

## Parameters and domain

canvas, dtype default Float64; finite ordered bounds xmin<=xmax and ymin<=ymax are unchecked caller premises. Equality gives empty area.

## Mathematical specialization

At pixel lower corner (x,y),
RN_T(max(0,min(x+1,xmax)-max(x,xmin))*max(0,min(y+1,ymax)-max(y,ymin))). Round the full
rational product once.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

O(area(Q)).

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Bounds (1/4,1/4,3/4,3/4) cover 1/4 of the unit pixel; equal bounds give zero coverage; integer translation preserves coverage. Reversed bounds are outside the production input domain. An oracle may diagnose them as a malformed reference request, without a runtime rejection requirement.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
