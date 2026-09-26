---
spec_schema_version: 1
id: NOI-04A
parent_id: NOI-04
function: perlin2002_3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.perlin2002_3d_v1_strict
  - noise.perlin2002_3d_v1_accelerated_apple_silicon
  - noise.perlin2002_3d_v1_accelerated_x86_64
---

# NOI-04A: perlin2002 3d

Inherit [NOI-04](NOI-04_gradient_noise_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,3] finite Float32/64; values[S...], rank>=1.

## Parameters and domain

dtype default Float64; no seed/frame/custom permutation; period 256 on each axis.

## Mathematical specialization

Exact floor and modulo256; fade(f)=6f^5-15f^4+10f^3. Use the fixed 256-entry permutation
and grad(hash&15): u=x when h<8 else y; v=y when h<4, x for h=12/14, otherwise z; bits
0/1 choose signs. Sum all eight weighted corner dots before RN. See
NOI_perlin2002_permutation.md.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional coordinate samples, O(8*count(Q)); arbitrary finite coordinate reduction is
exact.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Integer lattice +0, period256, negative and huge coordinates, independent Fraction
polynomial; conservative abs(noise)<=2, no strict [-1,1] claim or Java bit identity.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
