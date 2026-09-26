---
spec_schema_version: 1
id: PTH-05A
parent_id: PTH-05
function: width_profile_linear
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.width_profile_linear_v1_strict
  - path.width_profile_linear_v1_accelerated_apple_silicon
  - path.width_profile_linear_v1_accelerated_x86_64
---

# PTH-05A: width profile linear

Inherit [PTH-05](PTH-05_width_profiles_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

s[N], associated source lengths/domain selection,stops[K],widths[K]; values[N].

## Parameters and domain

Explicit pathset_normalized_arclength/subpath_normalized_arclength/subpath_arclength_px;
K>=2, increasing stops, finite nonnegative widths. Normalized stops cover0..1; px stops
cover source length. Source index/mapping fields require formal schema.

## Mathematical specialization

Choose correct source whole/subpath length L and source s. Normalize exactly as u=s/L
(L0 uses0) or use u=s. Hits copy/convert width; otherwise interpolate full rational
formula then RN. No hidden extrapolation/clamping. Actual domain endpoints and
zero-length association behavior must match the formal binding.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries, full profile controls; O(K+N log K), exact source mapping.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Source lengths100/300 distinguish the three domains; trimmed25..75 retains widths4..8
for original2..10 profile; negative widths fail.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
