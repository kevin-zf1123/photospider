---
spec_schema_version: 1
id: NOI-10C
parent_id: NOI-10
function: stbn_rank_volume
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
implementation_scope: generic_rank_resource
qualified_asset_status: not_provided
operation_keys:
  - noise.rank_volume_v1_strict
---

# NOI-10C: stbn rank volume

Inherit [NOI-10](NOI-10_temporal_contract.md), [GEN common](GEN_common_contract.md), and
the applicable [random](NOI_random_contract.md) and [geometry](PTH_geometry_contract.md)
contracts. A key includes algorithm version
and profile; within one version/profile, output bits and discrete decisions are fixed.

## Runtime implementation

The registered CPU key `noise.rank_volume_v1_strict` implements generic rank-volume
lookup. Descriptor parameters and the SDK descriptor helper follow NOI-07A, with
N=T*Th*Tw<=2^40 and byte length 8N. Height, width and signed Int64 frame are required.
Origins and offsets x/y/t default to zero; dtype defaults to float64. Widened integer
sums avoid signed-64 overflow before Euclidean modulo. Selected tiles read only
their addressed frame slice and periodic spatial support. Payload permutation and
content binding are caller premises; midpoint rounding is exact. `max_work`
bounds actual work. No named STBN-quality or GPU key is registered, and no qualified
STBN volume is bundled.

## Interface and representation

rank[T,Th,Tw] Int64 and immutable resource descriptor; values[H,W].

## Parameters and domain

frame and offsets x/y/t Int64; canvas/dtype. Generic volume lookup checks static structure; valid payload and identity are unchecked premises; named STBN additionally requires approved provenance/license and
spatial/temporal reports.

## Mathematical specialization

complete-input volume is a permutation0..N-1, N=T*Th*Tw. Address frame+offset_t modulo T and
global xy plus offsets modulo spatial shape; use NOI-07A midpoint mapping. This frame is
a lookup coordinate, not the unsigned32 RNG frame field. SDK formats require explicit
conversion and requalification.

## Dependencies, resources and failures

Execution support: **Regional**. Lower each selected complete output tile to
fixed tasks with explicit query/sample and control-member tile reads. Close each
read on its source grid; payload validity over the consumed support is a caller
premise. Empty output selection performs static preparation without payload tasks.
Shared computation uses explicit task dependencies and private intermediate
Results under GEN-common.

Regional lookup without scanning volume permutation/content, O(area(Q)); periodic T.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Negative frame, period T, valid rank permutations and explicit content identity; spatial-blue/time-white
fixtures cannot certify STBN.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. No unverified GPU or third-party bit
identity is implied by these requirements.
