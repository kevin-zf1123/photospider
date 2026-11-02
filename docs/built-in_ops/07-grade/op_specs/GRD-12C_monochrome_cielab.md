---
spec_schema_version: 1
id: GRD-12C
parent_id: GRD-12
function: monochrome_cielab
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.monochrome_cielab
---

# GRD-12C: CIELAB monochrome

Inherits the [family contract](GRD-12_monochrome_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.monochrome_cielab` performs the transformation below.

## Interface

`input: T[S,C]` -> `values: T[S,C]`.

Select relative linear RGB with explicit basis/white. Output replaces the same RGB group; other slots, including alpha, bypass.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

Resolve `source_basis` and the relative linear RGB selection as in GRD-12B.

## Mathematics

Convert through the common `cielab_l` route in Float64, retain the native l/L coordinate, replace a,b by zero and convert back to the source linear RGB basis. This reconstructs neutral color in the selected model rather than treating l/L as a linear RGB value.

### Current native algorithm/profile

Compute relative Y in Float64, then explicitly publish l = 1.16*f(Y)-0.16 in Float64. Form f_y=(l+0.16)/1.16 and y_n=f^-1(f_y), and reconstruct inverse(M)*(W_x*y_n,y_n,W_z*y_n). The published lightness stage is retained rather than algebraically cancelling the Lab round trip. Native matrix dot products may use fused arithmetic; final RGB is converted once to T.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Black remains black. Compare a colored input to explicit model-neutral reconstruction, and check alpha/AOV bit preservation. CIELAB l=0.5 corresponds to relative Y=(66/116)^3.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
