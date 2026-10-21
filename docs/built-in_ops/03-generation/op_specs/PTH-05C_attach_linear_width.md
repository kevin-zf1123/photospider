---
spec_schema_version: 1
id: PTH-05C
parent_id: PTH-05
function: attach_linear_width
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_workflow_keys:
  - path.attach_linear_width_v1_strict
---

# PTH-05C: attach linear width

Inherit [PTH-05](PTH-05_width_profiles_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet, stops[K], widths[K] Float64 and explicit domain/binding fields; return the unchanged complete PathSet value paired with a photospider.ops.path_width v1 Result.

## Parameters and domain

Three domains as PTH-05A; replace_existing default false, the absence of a conflicting width key is an unchecked caller premise;
the standard key is width and the shared geometry contract specifies its schema. Normalized stops0..1; px profile covers associated length.

## Mathematical specialization

Preserve geometry bits, attach scalar Linear width with explicit source domain and rebuild associations. The v1 attached schema represents Linear width only; a generic ArcLength label cannot substitute for the three specified source domains. Independent PCHIP consumers use an explicit profile or separately specified bounded conversion.

## Dependencies, resources and failures

Computation scope: **complete input in a Run-shared stage**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, following the declared dependencies and static structure checks.

Construct the width metadata from explicit inputs in O(bindings+K), recording source IDs without validation. Return the original geometry value directly, preserving its identity and avoiding a copy.

All output, scratch, indices, algorithmic work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve static structural errors and operational NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Three domains, valid source snapshot pairing and unchanged geometry bits.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. The specified schemas, member algorithms and backend implementations must be delivered and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
