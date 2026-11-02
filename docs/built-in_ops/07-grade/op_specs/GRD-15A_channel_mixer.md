---
spec_schema_version: 1
id: GRD-15A
parent_id: GRD-15
function: channel_mixer
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.channel_mixer
---

# GRD-15A: Matrix channel mixer

Inherits the [family contract](GRD-15_channel_mixer_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.channel_mixer` performs the transformation below.

## Interface

`input: T[S,C]`, `matrix: Float64[D,Csel]`, `bias: Float64[D]` -> `values: T[S,D]`.

1<=Csel,D<=64. Explicit ordered source slots and destination component roles. Matrix and bias are required; identity and zero are suggested when Csel=D.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The registered key is `grade.channel_mixer`. Its three ordered dynamic ports are `input`, `matrix` and `bias`; its output port is `values`.

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Required for raw selection; zero-based component axis within the tensor cell, excluding batch axes. |
| `components` | String | Required with `axis`; canonical comma-separated distinct source slot indices, in matrix-column order, with 1..64 entries. |
| `input_description` | String | Optional existing FMT tensor-description encoding; resolves the component axis and an explicitly named group in place of raw selectors. Native numeric encoding is required. |
| `group` | String | Required with `input_description`; the group's ordered indices define matrix-column order. |
| `output_roles` | String | Required comma-separated nonempty role names, one for each of the D output rows. Names contain no whitespace or commas. They declare row meaning; complete color interpretation is bound explicitly by downstream consumers. |
| `max_work` | Int64 | Optional per-invocation work limit; default 9223372036854775807, minimum zero. |

The component axis is retained with extent D, and batch and sample axes retain their order. Raw selectors and description selectors are mutually exclusive. Spatial tensors use their declared component axis.

## Mathematics

y_j=sum_c M[j,c]*x_c+bias[j]. Output contains exactly the D calculated channels in row order. An exact one-hot row with zero bias selects the corresponding source value.

### Current native algorithm/profile

Matrix entries, bias and the accumulation are Float64 for either image dtype. For each non-copy row, initialize with the first selected product, accumulate the remaining products in selected-column order using native fused multiply-add, add the bias, and convert once to T. Exact one-hot rows with zero bias copy the selected source bits, including signed zero and NaN payloads. Output values are unclipped. Matrix and bias payloads are consumed for the requested output rows at invocation time.

## Behavior

Each output row depends on its selected source tuple and coefficient row. Alpha or AOV inclusion requires explicit rows or separate assembly.

## Acceptance

The R/B permutation matrix swaps channels. A row [0.5,0.5,0] averages R and G.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
