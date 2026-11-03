---
spec_schema_version: 1
id: GRD-17A
parent_id: GRD-17
function: invert
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.invert
---

# GRD-17A: Reflect a numeric range

Inherits the [family contract](GRD-17_invert_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.invert` performs the transformation below.

## Interface

`input: T[S,C]`, `bounds: Float64[Csel,2]` [lo,hi] -> `values: T[S,C]`.

lo<hi. Suggested bounds [0,1]. Input may extend beyond these bounds.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The registered key is `grade.invert`. Its ordered dynamic ports are `input` and `bounds`; both are required. The output port is `values`. Each bounds row corresponds to the same position in the ordered selected-component list.

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required for raw selection; zero-based component axis within the tensor cell, excluding batch axes. |
| `components` | String | Required with `axis`; a nonempty list of distinct canonical comma-separated slot indices. The list order determines bounds-row order. |
| `input_description` | String | Optional existing FMT tensor-description encoding; supplies the component axis and selection through an explicitly named group. |
| `group` | String | Required with `input_description`; selects that group's ordered components. |
| `max_work` | Int64 | Optional per-invocation work limit; default 9223372036854775807, minimum zero. |

Raw selectors and description selectors are mutually exclusive. Description selection validates FMT structure and uses the named group's indices in their declared order. Profile/configured descriptions do not require an analytic endpoint binding for this numerical operator. The description contributes structural selection only; bounds and stored sample values determine the arithmetic. The bounds tensor has no batch axes. Bounds are dynamic for every invocation; neither bounds nor image data are implicitly broadcast.

## Mathematics

y=lo+hi-x. Results remain unclipped. Applying twice is an algebraic identity subject to floating error.

### Current native algorithm/profile

For each selected component, form `s=lo+hi` in Float64, then compute `s-Float64(x)` and convert once to T. Unselected components preserve their original bits. The arithmetic follows the common IEEE overflow and gradual-underflow behavior; a zero bound sum still uses subtraction and does not introduce a unary-negation or copy rule. Empty requests consume no payload, and bypass-only requests consume no bounds.

For Float32 output, a finite `s` that is exactly representable in Float32 permits an equivalent binary32 subtraction: two binary32 operands produce the same final binary32 result whether their difference is evaluated directly or through binary64. Other bound sums retain the Float64 intermediate. This equivalence does not change the calculation profile or introduce tile-dependent rounding.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

[0,0.25,1,2] with [0,1] gives [1,0.75,0,-1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
