---
spec_schema_version: 1
id: PTH-10A
parent_id: PTH-10
function: stroke_constant_area
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.stroke_constant_area_v1_strict
  - path.stroke_constant_area_v1_accelerated_apple_silicon
  - path.stroke_constant_area_v1_accelerated_x86_64
---

# PTH-10A: stroke constant area

Inherit [PTH-10](PTH-10_stroke_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

Polyline PathSet,constant width[1]>=0 in px; coverage[H,W].

## Parameters and domain

canvas/dtype; cap=round default/butt/square, join=round default/bevel/miter;
miter_limit>=1,constructor4; closed paths ignore caps.

## Mathematical specialization

Radius r=width/2 exactly. Each nonzero segment contributes the rectangle between its
normal offsets +/-r. At an open endpoint round adds a semicircle, butt adds nothing, and
square extends the rectangle by r along the endpoint tangent. Inner joins are covered by
the union of strips. Outer round joins add the circular sector; bevel joins add the
triangle connecting outer strip corners; miter joins extend the outer side lines to
their intersection. Union these regions before pixel-area RN. Miter compares exact outer
distance/radius to limit, equality keeps miter, excess bevels. At a 180-degree reversal
round adds a semicircle; bevel/miter do not produce an infinite spike. Zero width empty;
isolated point round=disk,square=axis-aligned width square,butt=empty.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional pixels; exact line/circular boundary arrangement and certified area, with
global geometry controls.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Butt rectangle, point disk/square, acute miter threshold, overlap not darkened, subpixel
coverage.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
