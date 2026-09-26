---
spec_schema_version: 1
id: PTH-07A
parent_id: PTH-07
function: trim_path
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.trim_path_v1_strict
---

# PTH-07A: trim path

Inherit [PTH-07](PTH-07_trim_dash_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Core/Bezier PathSet,start[1],end[1]; trimmed CoreVerbs PathSet with source mappings.

## Parameters and domain

units=normalized_arc/arc_px; each subpath validates its range; closed_wrap default
false; attribute_policy.

## Mathematical specialization

Convert endpoints to true subpath arc positions. a=b yields empty. Ordinary0<=a<=b<=L;
closed explicit wrap with a>b emits[a,L] then[0,b] as one open subpath. Full range
preserves original closed state/geometry bits. Invert boundaries, exact de Casteljau
trim then RN64 controls; verify connections/publication. Preserve source-position width
using source snapshot and original lengths; explicit rebind alone reapplies full
profile.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(segments+inverse work); maps must survive publication rounding or reject/drop,
never guessed interpolation.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

100px line25..75, empty interval, closed wrap, full-range bit identity and three width
domains.

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
