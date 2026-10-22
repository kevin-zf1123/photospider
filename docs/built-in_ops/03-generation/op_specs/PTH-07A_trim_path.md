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

units=normalized_arc/arc_px; each subpath range is valid as an unchecked premise; closed_wrap default
false; attribute_policy.

## Mathematical specialization

Convert endpoints to true subpath arc positions. a=b yields empty. Ordinary0<=a<=b<=L;
closed explicit wrap with a>b emits[a,L] then[0,b] as one open subpath. Full range
preserves original closed state/geometry bits. Invert boundaries, exact de Casteljau
trim then RN64 controls; connection and publication geometry are unchecked caller
premises, with no completed-output validation. Preserve source-position width
using source snapshot and original lengths; explicit rebind alone reapplies full
profile.

## Dependencies, resources and failures

Execution support: **complete input**. Lower the complete mathematical input
support and required output member tiles into explicit tasks. Share global work
through private intermediate Results and fixed dependencies. Required members
publish on successful call retirement. Empty output selection performs static
preparation without payload tasks; there is no runtime dependency discovery or
dirty propagation.

complete-input O(segments+inverse work). Source-map reject/drop applies only
when the selected map representation cannot express the computed mapping, such
as a non-rational arc cut in the v1 rational affine map. Rounding compatibility,
connections and topology remain unchecked premises; there is no publication
scan or guessed interpolation.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural failures and actual
resource, cancellation, backend and upstream statuses. Member-specific no-solution,
refinement or quality failures apply to valid inputs and the explicitly declared
algorithm; numerical-premise violations have no prescribed runtime diagnostic.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

100px line25..75, empty interval, closed wrap, full-range bit identity and three width
domains.

Check selected-tile/full-output equivalence, source-grid read closure, legal
strides/offsets, separate/joint outputs, version/profile identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
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
