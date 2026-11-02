---
spec_schema_version: 1
id: GRD-13A
parent_id: GRD-13
function: posterize
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.posterize
---

# GRD-13A: Uniform posterization

Inherits the [family contract](GRD-13_posterize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.posterize` performs the transformation below.

## Interface

`input: T[S,C]` -> `values: T[S,C]`.

Selected normalized components; static levels:Int64 2..65536, default 8; edge=reject|clamp, default clamp.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

`levels: Int64` defaults to 8 and `edge: String` defaults to `clamp`. Raw selection uses descriptor-relative `axis: Int64` and comma-separated `components: String`; a serialized `input_description: String` with `group: String` may resolve the selected components instead.

## Mathematics

Set u=x in reject mode, otherwise clamp(x,0,1). Let q=round_ties_even(u*(levels-1)); output q/(levels-1). The product and division use native floating arithmetic. Zero and one return exactly 0 and 1.

### Current native algorithm/profile

Cast the integer count levels-1 to T. Both the product u*T(levels-1) and division by T(levels-1) are evaluated in T; no Float64 arithmetic operand is introduced by the integer parameter. Round the product to the nearest even integer, independently of a floor(x+0.5) approximation. The scalar form uses floor, the fractional remainder and integer parity; the packet form uses the same nearest/even operation. Bypass slots retain their original bits.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

levels=2 maps 0.5 to 0; levels=3 maps 0.5 to 0.5. It differs from an x>=0.5 binary threshold.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
