---
spec_schema_version: 1
id: PTH-01B
parent_id: PTH-01
function: concat_paths
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.concat_paths_v1_strict
---

# PTH-01B: concat paths

Inherit [PTH-01](PTH-01_construction_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

K>=1 PathSet inputs, fixed compile-time arity; one PathSet.

## Parameters and domain

Identical coordinate systems and geometry authority; explicit conversion for mixed
authorities; attribute_policy default reject_unmappable.

## Mathematical specialization

Concatenate subpaths in input-port order without connecting equal endpoints. Reindex
offsets and associations. Path-constant attributes merge only with identical
values/descriptions. Arc-domain bindings must map using actual source lengths and
explicit parameter domain; otherwise reject or explicitly drop.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole O(total rows), plus required length/mapping work.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Equal endpoints remain separate subpaths, empty inputs, incompatible authority,
concat/split geometry roundtrip, unmappable attributes.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
