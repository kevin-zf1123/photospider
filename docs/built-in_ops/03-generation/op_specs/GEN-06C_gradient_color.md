---
spec_schema_version: 1
id: GEN-06C
parent_id: GEN-06
function: gradient_color
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
---

# GEN-06C: gradient color

Inherit [GEN-06](GEN-06_gradient_lookup_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. Proposed keys are not runtime
registrations. A key includes algorithm version and profile; within one version/profile,
output bits and discrete decisions are fixed.

## Interface and representation

t of shape S, stops[K], colors[K,C]; complete described colors S+[C], an image when
S=HW.

## Parameters and domain

Separate RGB/XYZ/CMYK/Lab/LCh/OKLab/OKLCh/HSL/YCbCr members; strictly increasing stops,
complete FMT description; explicit upstream spread.

## Mathematical specialization

Compose corresponding CRV-06/FMT rules after migration. For RGB interpolate in linear
light: Q=(1-w)*a0*D(c0)+w*a1*D(c1), A=(1-w)*a0+w*a1; straight result E(Q/A) for A>0,
transparent black at A=0. Preserve CRV endpoint/AssociationUnderflow rules. Lab
l=L*/100; original hue and winding are interpolated, not normalized.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional query target; composed producer execution retains its real dependency rules and
public rounding stages.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Transparent red to opaque blue midpoint is blue with alpha 0.5; Lab/OKLab units;
multi-turn hue; positive-alpha underflow failure.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it is
not runtime completion. Formal schemas, unresolved algorithms and backend admission must
be implemented and validated before registration. No unverified GPU or third-party bit
identity is implied by these requirements.
