---
spec_schema_version: 1
id: PTH-07B
parent_id: PTH-07
function: dash_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.dash_path_v1_strict
---

# PTH-07B: dash path

Inherit [PTH-07](PTH-07_trim_dash_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
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

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole; charge dash count, true inverse and source mappings; hard limits never erase tiny
dashes.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

L10 pattern3,2 gives[0,3],[5,8]; offset1 gives[0,2],[4,7],[9,10]; odd3 equals3,3;
all-zero fails; closed seam.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.

## Source width mapping

For every output fragment retain source snapshot, subpath and ordered source arc
intervals. Before publication, local fragment arc s maps to source a+s; a wrapped
fragment uses [a,L] then [0,b]. After rounded-control publication retain the exact
source parameter mapping rather than equating new arc length with old arc length.
Per-subpath pixel domain queries source s; normalized subpath queries s/source_L;
whole-PathSet normalized queries (source_prefix+source_s)/source_total_L. Attached and
independent width use the same mapping. If the formal representation cannot express it,
reject_unmappable fails; explicit drop removes only unmappable fields and reports
keys/reasons. No guessed interpolation or implicit rebind.
