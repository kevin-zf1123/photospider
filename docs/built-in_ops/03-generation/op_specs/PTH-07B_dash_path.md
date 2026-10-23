---
spec_schema_version: 1
id: PTH-07B
parent_id: PTH-07
function: dash_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - path.dash_path_v1_strict
---

# PTH-07B: dash path

Inherit [PTH-07](PTH-07_trim_dash_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet,nonnegative pattern[K],finite offset[1] in px; dashed PathSet with
source mappings.

## Parameters and domain

K>=1,at least one positive entry; restart=per_subpath; max_output_segments;
attribute_policy.

## Mathematical specialization

Duplicate odd patterns, even entries on/odd off. P=sum(pattern),
q=offset-P*floor(offset/P); phase=(s+q) mod P. Half-open intervals, skip zero entries
without zero-length dashes. Closed seam-connected on pieces join into one open dash;
only all-on retains closed. Each piece uses PTH-07A and preserves original width
positions, not a fresh normalized profile.

## Runtime implementation

`path_trim.cpp` implements dash phase, zero-entry skipping, odd-pattern duplication and closed-seam joining before applying the same certified restriction machinery as PTH-07A. It publishes the same three outputs and uses the same source-mapping policy and hard budgets. The algorithm retains the original source position across all emitted fragments.

Dash boundaries remain exact rational pixel-arc targets through total-length comparisons and the shared PTH-07A inverse. Exact analytic relations resolve a boundary that coincides with a segment junction or the full subpath length; no rounded-length comparison decides whether to emit or join a dash.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input; charge dash count, true inverse and source mappings; hard limits never erase tiny
dashes.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

L10 pattern3,2 gives[0,3],[5,8]; offset1 gives[0,2],[4,7],[9,10]; odd3 equals3,3;
at least one positive pattern entry is assumed; closed seam.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The registered CPU implementation uses the bounded algorithms and failure rules specified here. No unverified GPU or third-party bit
identity is implied by these requirements.

## Source width mapping

For every output fragment retain source snapshot, subpath and ordered source arc
intervals. Before publication, local fragment arc s maps to source a+s; a wrapped
fragment uses [a,L] then [0,b]. After rounded-control publication retain the exact
source parameter mapping rather than equating new arc length with old arc length.
Per-subpath pixel domain queries source s; normalized subpath queries s/source_L;
whole-PathSet normalized queries (source_prefix+source_s)/source_total_L. Workflow-paired path_width Results and
independent width inputs use the same mapping. If the formal representation cannot express it,
reject_unmappable fails; explicit drop removes only unmappable fields and reports
keys/reasons. No guessed interpolation or implicit rebind.
