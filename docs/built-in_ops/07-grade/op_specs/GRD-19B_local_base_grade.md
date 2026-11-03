---
spec_schema_version: 1
id: GRD-19B
parent_id: GRD-19
function: local_base_grade
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.local_base_grade
---

# GRD-19B: Adjust a supplied local tonal base

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.local_base_grade` performs the transformation below.

## Interface

`input: T[S,C]`, `base: Float64[S,1]`, `controls: Float64[4]` [pivot_log,contrast,shadow_ev,highlight_ev], `edges: Float64[4]` -> `values: T[S,C]`.

Linear RGB/Gray. contrast>=0; suggested controls [-2,1,0,0]. Edges satisfy GRD-18A and use stops; suggested [-8,-4,-1,2].

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Static parameters

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Explicit component axis for raw tensor selection. |
| `components` | String | Required with `axis`; one Gray slot or three distinct comma-separated slot indices in R,G,B order. |
| `input_description` | String | Existing FMT tensor-description encoding selecting explicit linear RGB or linear-Y Gray coordinates. |
| `group` | String | Required with `input_description`; selected color group, with component order resolved from roles. |
| `max_work` | Int64 | Optional per-invocation work limit, minimum zero; default 9223372036854775807. |

Raw selectors and description selectors are mutually exclusive. `base` has the same ordered sample axes as `input`, with the selected component-axis extent replaced by one. Controls and edges are dynamic Float64 tensors.

## Mathematics

Evaluate shadow/highlight weights from base by GRD-18A. e=(contrast-1)*(base-pivot_log)+shadow_ev*w_shadow+highlight_ev*w_highlight. Apply x*exp2(e). contrast=1 with both EV controls zero copies the input.

### Current native algorithm/profile

Compute zone weights and exposure in Float64 using the GRD-18A endpoint-aware smoothstep. A zero `(contrast-1)` coefficient or zero zone coefficient bypasses its unused term. A complete identity or a native per-pixel `e==0` copies the original source bits. Otherwise form a Float64 `exp2(e)` gain, multiply each selected source component in Float64, and convert once to T; unselected components copy their original bits.

For `e` in [-1000,1000], the native vector gain uses `k=round(64*e)`, `r=(e-k/64)*ln(2)`, a rounded Float64 `2^((k mod 64)/64)` table, a degree-five `exp(r)` polynomial and binary exponent scaling. Other arguments retain the full-domain native `exp2`. Singleton samples, packet tails and component-only tiles use the same exposure and gain evaluation as full tuples. A sample outside the regular gain range does not change the gain algorithm for other samples. At products within a relative margin of 2^-30 of the destination type maximum, including overflowing products, recompute the gain with the scalar library `exp2` before multiplying. The gain is formed before source multiplication, including overflow and underflow. The common native comparison criteria apply; no clipping or input-domain scan is added.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs copy their source bits. The family contract specifies the execution-tile scope of bypass-only dependencies.

## Acceptance

At base=pivot_log with zero zone EV, changing contrast leaves the pixel unchanged. A uniform +1 effective EV doubles the pixel. Compare whole tuples and component-only tiles at Float64 overflow boundaries, and compare singleton and multi-sample requests when exposure terms cancel to zero.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
