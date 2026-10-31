---
spec_schema_version: 1
id: GRD-01A
parent_id: GRD-01
function: exposure
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.exposure
---

# GRD-01A: Uniform linear exposure

Inherits the [family contract](GRD-01_exposure_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.exposure` performs the transformation below.

## Interface

`input: T[S,C]`, `ev: Float64[1]` -> `values: T[S,C]`.

Select linear RGB or linear-Y Gray components. The EV unit is stops; the suggested control value is 0.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The operation key is `grade.exposure`. EV is the second dynamic input, not a static parameter. The current parameter representation specializes the common draft as follows:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Raw mode: required component axis in the tensor cell descriptor, excluding the `batch_axes` prefix; `0 <= axis < cell_rank`. |
| `components` | String | Raw mode: required canonical comma-separated indices, for example `"0"` or `"0,1,2"`. Exactly one Gray component or three RGB components, distinct and inside `axis`. This String representation replaces the common draft's `Array[Int64]` notation for this member. |
| `input_description` | String | Semantic mode: canonical FMT tensor-description parameter encoding. Resolves an explicit component axis and linear RGB or linear-Y Gray interpretation. |
| `group` | String | Semantic mode: required name of the selected complete color group. RGB roles are `red`, `green`, `blue`; the Gray role is `gray`. |
| `max_work` | Int64 | Optional nonnegative work bound, default `INT64_MAX`; it does not alter the mathematical result. |

Use either the raw `axis`/`components` pair or semantic `input_description`/`group`, never both. Raw mode asserts the stated linear color domain without inferring it from channel count. The current prototype requires a component axis in both modes; Gray requires an explicit component axis, with extent one for Gray alone; unselected AOV components may share a larger component axis. Batch axes are preserved, and the resolved component axis follows their prefix. Components outside the selection are independent bit-preserving bypasses; a fourth stored component is not implicitly selected as alpha.

## Mathematics

`y = x * exp2(ev)` for each selected component. EV zero copies the input. Zero x preserves its sign for every finite EV, including when exp2(ev) would overflow.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Input 0.25 at +1 stop gives 0.5; at -1 stop gives 0.125. EV zero preserves signed/HDR samples and bypass bits.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
