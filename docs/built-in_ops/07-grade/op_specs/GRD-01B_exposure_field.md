---
spec_schema_version: 1
id: GRD-01B
parent_id: GRD-01
function: exposure_field
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.exposure_field
---

# GRD-01B: Exposure from a spatial EV field

Inherits the [family contract](GRD-01_exposure_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.exposure_field` performs the transformation below.

## Interface

`input: T[S,C]`, `ev: Float64[S,1]` -> `values: T[S,C]`.

Linear RGB or linear-Y Gray. The field uses stops and exactly matches S; its value is shared over the selected color components.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The operation key is `grade.exposure_field`. EV is the second dynamic input, not a static parameter. The current parameter representation specializes the common draft as follows:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Raw mode: required component axis in the tensor cell descriptor, excluding the `batch_axes` prefix; `0 <= axis < cell_rank`. |
| `components` | String | Raw mode: required canonical comma-separated indices, for example `"0"` or `"0,1,2"`. Exactly one Gray component or three RGB components, distinct and inside `axis`. This String representation replaces the common draft's `Array[Int64]` notation for this member. |
| `input_description` | String | Semantic mode: canonical FMT tensor-description parameter encoding. Resolves an explicit component axis and linear RGB or linear-Y Gray interpretation. |
| `group` | String | Semantic mode: required name of the selected complete color group. RGB roles are `red`, `green`, `blue`; the Gray role is `gray`. |
| `max_work` | Int64 | Optional nonnegative work bound, default `INT64_MAX`; it does not alter the mathematical result. |

Use either the raw `axis`/`components` pair or semantic `input_description`/`group`, never both. Raw mode asserts the stated linear color domain without inferring it from channel count. The current prototype requires a component axis in both modes; Gray requires an explicit component axis, with extent one for Gray alone; unselected AOV components may share a larger component axis. Batch axes are preserved, and the resolved component axis follows their prefix. Components outside the selection are independent bit-preserving bypasses; a fourth stored component is not implicitly selected as alpha.

## Mathematics

Apply [GRD-01A](GRD-01A_exposure.md) with ev[p] at each coordinate p. A zero field sample copies the corresponding source pixel.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Input [0.25,0.25] and EV [0,1] gives [0.25,0.5]. A crop uses the matching full-image EV coordinates.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
