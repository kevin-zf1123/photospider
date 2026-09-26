---
spec_schema_version: 1
id: PTH-03A
parent_id: PTH-03
function: bezier_arc_length
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.bezier_arc_length_v1_strict
  - path.bezier_arc_length_v1_accelerated_apple_silicon
  - path.bezier_arc_length_v1_accelerated_x86_64
---

# PTH-03A: bezier arc length

Inherit [PTH-03](PTH-03_arc_length_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet; independent Float64 length[1] and ArcLengthTable Result.

## Parameters and domain

epsilon_length_px>0 default1e-6; candidate max_depth24,max_table_rows1048576; no Float32
length.

## Mathematical specialization

Integrate speed exactly as target; total follows subpath order without jumps and
includes Z. Deterministic dyadic subdivision allocates epsilon/S across original
segments and halves leaf budgets on split, DFS left first. Chord lower/control-polygon
upper bounds enclose length. Store directed RN64 cumulative bounds. Both length outputs
equal RN64 true length, requiring additional refinement beyond table tolerance.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole length/table, separately requested; O(leaves*d^2), charged stack/table.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

3-4-5 length5, subdivided line100, collinear reversal, empty length0/table,
independently enclosed integrals; finite tolerance alone does not certify RN.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
