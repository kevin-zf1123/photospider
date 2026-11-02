---
spec_schema_version: 1
id: GRD-16B
parent_id: GRD-16
function: color_balance_opponent
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_balance_opponent
---

# GRD-16B: Tonal opponent-color balance

Inherits the [family contract](GRD-16_color_balance_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_balance_opponent` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `offsets: Float64[3,2]` -> `values: T[S,C]`.

Select CIELAB or OKLab. Zone conventions are as GRD-16A. Suggested offsets are zero, in native a/b units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The registered key is `grade.color_balance_opponent`. The ordered dynamic ports are `input`, `zones` and `offsets`; the output port is `values`. All three inputs are required.

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required for raw selection; zero-based component axis within the tensor cell, excluding batch axes. |
| `components` | String | Required with `axis`; three distinct canonical comma-separated slot indices in l/L,a,b order. |
| `model` | String | `cielab` or `oklab`; required for raw selection. With a description, defaults to its resolved native model and must agree when explicitly supplied. |
| `input_description` | String | Optional existing FMT tensor-description encoding; supplies the native model and component axis instead of raw selectors. |
| `group` | String | Required with `input_description`; selects a group whose roles resolve l/L,a,b order. |
| `max_work` | Int64 | Optional per-invocation work limit; default 9223372036854775807, minimum zero. |

Raw selectors and description selectors are mutually exclusive. The zones tensor has the same ordered sample axes as the input, with the selected component-axis extent replaced by three in shadow, midtone, highlight order. Batch dimensions participate in this exact shape correspondence. Offsets are an unbatched Float64[3,2] table with a,b columns. CIELAB uses native a*,b* units and normalized l; OKLab uses native a,b,L units. No color conversion or broadcasting is implied.

## Mathematics

Copy the native l/L coordinate. Add normalized weighted offsets to a and b using GRD-16A. This preserves stored model lightness.

### Current native algorithm/profile

For each adjusted coordinate, compute in Float64. Set `s=(w0+w1)+w2`, accumulate `n=fma(w2,o2,fma(w1,o1,w0*o0))` in shadow-to-highlight order, and set `delta=n/s`. A column of zero offsets copies the source coordinate. A native zero delta also copies the source coordinate; otherwise compute `Float64(source)+delta` and convert once to T. This is an unclipped native floating profile, including the common IEEE overflow and underflow behavior. The controls are dynamic values for the current invocation. Unadjusted components preserve their original bits.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

A nonzero a offset leaves l/L and b unchanged. Zero offsets preserve all source bits.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
