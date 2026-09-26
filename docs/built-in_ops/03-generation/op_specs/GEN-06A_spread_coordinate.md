---
spec_schema_version: 1
id: GEN-06A
parent_id: GEN-06
function: spread_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - generation.spread_coordinate_v1_strict
  - generation.spread_coordinate_v1_accelerated_apple_silicon
  - generation.spread_coordinate_v1_accelerated_x86_64
---

# GEN-06A: spread coordinate

Inherit [GEN-06](GEN-06_gradient_lookup_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

Floating t of any legal shape; same shape/dtype values.

## Parameters and domain

method=pad/repeat/reflect; finite inputs; no canvas.

## Mathematical specialization

pad clamps to [0,1] with NUM copy/signed-zero rules. repeat=t-floor(t);
reflect=1-abs(1-(t-2*floor(t/2))). Exact boundary decisions, then normal RN. Repeat
rounding to 1 stays 1; downstream lookup treats it as endpoint. Exact periodic boundary
is +0.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional pointwise demand.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

For -0.25,0,1,1.25, repeat gives 0.75,0,0,0.25 and reflect gives 0.25,0,1,0.75. Pad
preserves input -0.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
