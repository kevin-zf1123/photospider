---
spec_schema_version: 1
id: GRD-22B
parent_id: GRD-22
function: color_wheels_rgb
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_wheels_rgb
---

# GRD-22B: RGB tonal wheels

Inherits the [family contract](GRD-22_split_tone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_wheels_rgb` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `controls: Float64[3,3,2]` [ev,offset] by zone and RGB component -> `values: T[S,C]`.

Select linear RGB. Zones use GRD-16A conventions. Suggested controls zero; offsets are in source units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Let W=sum(w_z), E_c=sum(w_z*ev[z,c]) and O_c=sum(w_z*offset[z,c]), with native sums accumulated in ascending z order. A component bit-copies its source exactly when the raw sums E_c==0 and O_c==0, including dynamically cancelling or zero-weight controls. Otherwise form e_c=E_c/W and o_c=O_c/W in Float64, then compute y_c=x_c*exp2(e_c)+o_c and convert once to T. Normalization that underflows e_c or o_c to zero does not introduce another copy case.

The current native batch profile processes at most 64 RGB pixels, hence 192 independent normalized EV values, at a time and retains their raw copy flags. Its RGB batching is distinct from the tonal Gray profile's 256-value batch. It does not mix precomputed per-zone gains. A vector gain is admitted only for e_c in [-1022,1023], a finite result, product/output magnitudes below their type limits by a 2^-30 margin, and `abs(product)*2e-14 <= (atol+rtol*abs(result))/8`, using the unchanged common output tolerances. Other cases evaluate the standard library exponential and preserve the original gain, then multiply, then add sequence, including active overflow and cancellation. Neither normalized-zero controls nor exceptional neighbouring lanes alter the raw-copy rule. Unselected components and AOVs remain bit copies.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

A +1 red EV in a one-hot zone doubles red alone. All-zero controls preserve RGB and alpha.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
