---
spec_schema_version: 1
id: GRD-11C
parent_id: GRD-11
function: white_balance_temperature_tint
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.white_balance_temperature_tint
---

# GRD-11C: White balance from temperature and tint

Inherits the [family contract](GRD-11_white_balance_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.white_balance_temperature_tint` performs the transformation below.

## Interface

`input: T[S,C]` -> `values: T[S,C]`.

Select linear RGB. Required static source_temperature:Float64 in [1000,20000] kelvin; source_tint:Float64 in [-0.03,0.03] default 0. target_white defaults to the working RGB white; adaptation=xyz_scaling|bradford|cat02|cat16 defaults bradford.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

`source_temperature: Float64` is required. `source_tint: Float64` defaults to 0; `adaptation: String` defaults to `bradford`; optional `target_white: String` defaults to the source RGB basis white. Resolve `source_basis` and selection as in the family profile.

## Mathematics

Construct the source white from the Planckian CCT/tint model in [GRD-11](GRD-11_white_balance_contract.md). Adapt it to target_white using GRD-11B with the same source/destination RGB basis. source_temperature describes the illuminant being corrected; positive source_tint shifts that illuminant toward positive CIE 1960 v. The adaptation compensates that cast.

### Current native algorithm/profile

Evaluate the 471 sampled Planck terms and their analytic temperature derivatives in Float64, using expm1 for the exponential denominator and a common cancelling spectral scale. Normalize the UCS perpendicular with hypot and orient its v component positively before applying tint. Publish W = (1.5*u/v, 1, (4-u-10*v)/(2*v)) to Float64, then prepare the same effective adaptation matrix as GRD-11B. The sampled illuminant, target white and effective matrix depend only on the static parameters.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

When the constructed source white equals target_white, copy RGB. A source-white neutral maps to the target white under the selected adaptation.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
