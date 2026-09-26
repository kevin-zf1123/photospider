---
spec_schema_version: 1
id: GEN-01B
parent_id: GEN-01
function: constant_image
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.constant_image_v1_strict
  - generation.constant_image_v1_accelerated_apple_silicon
  - generation.constant_image_v1_accelerated_x86_64
---

# GEN-01B: constant image

Inherit [GEN-01](GEN-01_constant_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. Proposed keys are not runtime registrations. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

color[C] Float32/64 with explicit group/alpha description; image[H,W,C] of the same
dtype.

## Parameters and domain

canvas and complete FMT description; constructor may explicitly write linear
sRGB/D65/relative with encoding ranges.

## Mathematical specialization

Validate the complete finite color and alpha in [0,1], then copy each component.
Preserve finite hidden color at alpha=0. Publish straight planar image metadata; do not
premultiply or infer roles from C.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

O(C*area(Q)); full color controls validated.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

No alpha, non-last alpha slot, hidden color, Lab l=0.5 and invalid alpha.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
