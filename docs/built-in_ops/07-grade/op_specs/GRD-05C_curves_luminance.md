---
spec_schema_version: 1
id: GRD-05C
parent_id: GRD-05
function: curves_luminance
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
registered_operation_keys:
  - grade.curves_luminance
---

# GRD-05C: Proportional RGB luminance curve

Inherits the [family contract](GRD-05_curves_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.curves_luminance` performs the transformation below.

## Interface

`input: T[S,C]`, `knots: Float64[K]`, `ordinates: Float64[K,1]` -> `values: T[S,C]`.

Select relative linear RGB. Required method=linear|pchip; edge=reject|clamp|linear_extrapolate defaults reject; 2<=K<=65536. Curve coordinates use the `ratio` feature unit in the common brightness routes.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Extract the `ratio` feature in Float64. Evaluate the GRD-05A/B curve with Float64 output, and replace the target coordinate using [GRD-20F](GRD-20F_reconstruct_luminance.md). Each named feature/curve stage has a Float64 publication boundary. An unchanged mapped coordinate copies original RGB.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Native fast numerical profile

This key follows the native numerical [family profile](GRD-05_curves_contract.md#native-fast-numerical-profile). Curve evaluation and brightness feature boundaries use binary64, with final conversion to T. The strict/accelerated NUM rounding paths are not used.

## Acceptance

An identity curve preserves RGB when the published mapped coordinate equals the extracted coordinate. Test a nonidentity curve and retention of the route-specific color coordinates before inverse conversion.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
