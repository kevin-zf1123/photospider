---
spec_schema_version: 1
id: GRD-11B
parent_id: GRD-11
function: white_balance_adapt
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.white_balance_adapt
---

# GRD-11B: White-point adaptation of RGB

Inherits the [family contract](GRD-11_white_balance_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.white_balance_adapt` performs the transformation below.

## Interface

`input: T[S,C]` -> `values: T[S,C]`.

Required source RGB description, source_white and target_white xy, destination RGB description, and adaptation=xyz_scaling|bradford|cat02|cat16. Source and destination RGB are linear and use the same relative/absolute reference scale.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

`source_basis: String` and `destination_basis: String` use the FMT RGB basis codec. A described source may supply its basis through `input_description`; `output_description: String` may supply the destination basis and must retain the source selectors, linear reference and coordinate units. `source_white`, `target_white` and `adaptation` are required String parameters.

## Mathematics

Convert RGB to XYZ with the source basis M_s. Adapt with K=H^-1 diag((H*W_t)/(H*W_s)) H using the matrices in [FMT-10](../../02-format-color/op_specs/FMT-10_basis_math.md), and convert by M_d^-1. Whites have Y=1 and positive cone responses. Evaluate the matrix equations in native floating arithmetic. Identical bases and whites copy the selected RGB.

### Current native algorithm/profile

Resolve the RGB geometry and form the Float64 effective matrix A = inverse(M_d) * H^-1 * diag((H*W_t)/(H*W_s)) * H * M_s. Native matrix products and runtime RGB dot products may use fused multiply-add. Equal source/destination bases with equal adaptation whites use a bit-preserving identity.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

The source reference-white vector maps to the destination white. Equal basis/white inputs are bit-preserving identity.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
