---
spec_schema_version: 1
id: PTH-02E
parent_id: PTH-02
function: evaluate_pathset
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# PTH-02E: evaluate pathset

Inherit [PTH-02](PTH-02_evaluation_contract.md), [GEN common](GEN_common_contract.md),
and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

PathSet,segment_indices[N],t[N]; four evaluation outputs.

## Parameters and domain

Supported authorities/degrees inherit PTH-02A-D; Core IDs are verb rows (M invalid,Z
closing edge), primitive IDs are reference rows.

## Mathematical specialization

Dispatch by validated authority, preserving original segment IDs and explicit rounding.
Z runs from current point to M. No implicit global-parameter segment selection.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional queries with complete path association validation; requested position alone
need not compute tangent.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Mixed L/Q/C/Z, M query rejection, separate/joint outputs, empty path with query
NoSolution.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
