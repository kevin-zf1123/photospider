---
spec_schema_version: 1
id: PTH-12B
parent_id: PTH-12
function: offset_euclidean_region
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.offset_euclidean_region_v1_strict
---

# PTH-12B: offset euclidean region

Inherit [PTH-12](PTH-12_boolean_offset_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Polyline filled region and finite distance[1] in px; CoreVerbs offset outline and
GeometryReport.

## Parameters and domain

fill_rule; epsilon_outline_px>0
default0.05,epsilon_publication_px,max_events/max_segments; Euclidean round offset, no
miter/bevel aliases.

## Mathematical specialization

Positive d is Minkowski sum with closed disk(d); negative d is disk erosion; zero yields
canonical region. Compute actual exposed offset boundary, then explicit bounded arc
polygonization/publication. Negative offset may legitimately grow holes or remove
components. Preserve distinction between true target changes and approximation changes;
topology policy follows explicit outline rules.

## Runtime implementation

`path_stroke.cpp` constructs the Euclidean boundary band with `CircularArrangement`: dilation unions it with the regularized source region; erosion subtracts it and may remove the region entirely. Exact line/circle and circle/circle decisions precede bounded outline publication. `epsilon_outline_px`, stage-bound accumulation, deterministic subdivision and `topology_policy` follow PTH-10C. Nonzero circular offsets are not replaced by rational-only polygon joins.

The shared global outline budget includes straight offset edges and every retained split vertex. Publication error alone can make a requested outline impossible even when arc approximation is zero. Such a request fails before output publication; per-edge acceptance cannot substitute for the sum of the two global stage maxima.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; reliable line/circle constructions and offset arrangement required. Rational
polygon backend alone is insufficient; grid offset requires distinct semantics.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Disk radius change, rectangle rounded corners, erosion beyond half short side empty,
zero identity and certified neck separation. Eroding the unit square by the Float64 value `0.1` with `epsilon_outline_px=1e-20` must fail when the exact-to-Float64 vertex displacement exceeds that bound.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
