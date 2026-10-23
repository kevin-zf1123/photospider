---
spec_schema_version: 1
id: NOI-05A
parent_id: NOI-05
function: cellular2d_l2
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.cellular2d_l2_v1_strict
  - noise.cellular2d_l2_v1_accelerated_apple_silicon
  - noise.cellular2d_l2_v1_accelerated_x86_64
---

# NOI-05A: cellular2d l2

Inherit [NOI-05](NOI-05_cellular_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Runtime implementation

The three registered CPU keys above accept Float32/Float64 coordinate tuples and
publish independent `f1`, `f2`, and atomic `nearest_cell` outputs. `dtype`
defaults to `float64`; `seed` and `stream` are required, `frame` defaults to
zero, and `max_search_rings` defaults to 64 (rings 0 through the limit inclusive).
Outward distance bounds reject unambiguous candidates; overlapping bounds use exact
squared-distance ordering and the specified integer tie key. The exterior search
certificate is strict. Square roots preserve final RN32/RN64, including in the
accelerated profiles. `max_work` charges search and certification work. Exhausting
the ring limit without the F2 certificate returns OperationFailed with the
NotConverged domain code `0x4e4f490001`. No GPU key is registered.

## Interface and representation

coordinates[S...,2]; f1,f2[S...], nearest_cell[S...,2] Int64.

## Parameters and domain

dtype,seed/stream/frame; signed32 lattice; L2 metric; max_search_rings default 64.

## Mathematical specialization

Each domain7 cell uses its integer x/y, channel=0 and draw=0 with caller frame. Halfopen53 offsets use words 0/1 for x/y, leaving words 2/3 unused, and are published RN64 with explicit
half-open-cell endpoint adjustment. Sort all supported sites by
(distance_squared,iy,ix). Expand Chebyshev rings until the unvisited-cell closure lower
bound is strictly greater than current F2; equality cannot discard a tie. All cells needed by the mathematical search must have representable signed32 addresses as an unchecked caller premise; the field is not truncated to repair an invalid address.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional queries; O(count(Q)*R^2) reference; charge rings and certification.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

F1<=F2, brute-force crosscheck with exterior certificate, negative cells and distance
ties; ring exhaustion fails.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
