---
spec_schema_version: 1
id: GEN-05F
parent_id: GEN-05
function: path_distance_coordinate
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# GEN-05F: path distance coordinate

Inherit [GEN-05](GEN-05_gradient_coordinates_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet and positive scale[1] in px; t[H,W].

## Parameters and domain

canvas/dtype; explicit distance_source=centerline_unsigned/fill_signed/stroke_signed;
provide required fill/stroke parameters.

## Mathematical specialization

Compose the corresponding PTH-11 member followed by NUM division. Preserve the
published-distance rounding stage; do not replace the workflow with one rounded
true-distance/scale expression.

## Runtime implementation

`generation::path_distance_coordinate` is the public C++ authoring helper in `photospider/ops/generation.hpp`. It chooses the explicit PTH-11 distance member, publishes its `distance`, broadcasts the positive scale (same dtype as the published distance), then invokes NUM division. `stroke_signed` additionally requires the explicit stroke width input. The returned workflow output preserves the distance rounding stage and supports all three declared distance sources; no monolithic GEN-05F registry key is introduced.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional distance output; full path controls may be required.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Distance 2 with scale 4 gives 0.5; open-path signed fill requires explicit fill
interpretation.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.
