---
spec_schema_version: 1
id: PTH-05C
parent_id: PTH-05
function: attach_linear_width
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - path.attach_linear_width_v1_strict
---

# PTH-05C: attach linear width

Inherit [PTH-05](PTH-05_width_profiles_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet,stops[K],widths[K] Float64 and explicit domain/binding fields; PathSet with
standard width binding.

## Parameters and domain

Three domains as PTH-05A; replace_existing default false, duplicate width key rejected;
formal key/schema pending. Normalized stops0..1; px profile covers associated length.

## Mathematical specialization

Preserve geometry bits, attach scalar Linear width with explicit source domain and
rebuild associations. Do not reinterpret legacy ArcLength or claim existing enum
supports PCHIP. Independent PCHIP consumers use an explicit profile or separately
specified bounded conversion.

## Dependencies, resources and failures

Execution rule: **Whole**. Read complete structural/global inputs and publish the
selected complete Result atomically. Full shared-control/topology/resource changes
conservatively invalidate dependent output; sample changes follow the actual dependency
map. Empty requests perform static preflight without payload reads or advancing a random
sequence. Compute only requested output mathematics, retaining required shared
validation.

Whole association publication, O(rows+K); geometry owner reuse must retain correct new
identities.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Duplicate keys, three domains, source snapshot mismatch and unchanged geometry bits.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
