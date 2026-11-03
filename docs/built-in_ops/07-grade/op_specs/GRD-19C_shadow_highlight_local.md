---
spec_schema_version: 1
id: GRD-19C
parent_id: GRD-19
function: shadow_highlight_local
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.shadow_highlight_local
---

# GRD-19C: Local shadows, highlights and detail

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.shadow_highlight_local` performs the transformation below.

## Interface

`input: T[H,W,C]`, `controls: Float64[4]`, `edges: Float64[4]`, `detail_gain: Float64[1]` -> `values: T[H,W,C]`.

Select relative linear RGB or Gray. Base parameters are as GRD-19A; controls/edges as GRD-19B; detail_gain>=0 with suggested 1.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Static parameters

`axis`, `components`, `input_description`, `group` and `max_work` follow GRD-19B. `radius`, `sigma_space`, `sigma_range` and `epsilon` follow GRD-19A. The optional String `source_basis` uses the existing FMT RGB-basis encoding or a named preset such as `srgb_rec709`; raw RGB requires this basis, while a complete RGB description can supply it. An explicit basis must agree with any described primaries and white. Gray uses its selected linear-Y component directly. Relative RGB units are `1` or `relative`; relative Gray also accepts `relative_luminance`. Controls, edges and detail gain are dynamic Float64 tensors.

## Mathematics

Extract linear Y in Float64, publish z=log2(max(Y,epsilon)) to Float64 and calculate the Float64 GRD-19A base b. Let e_base be the exposure from GRD-19B and e_detail=(detail_gain-1)*(z-b). Return x*exp2(e_base+e_detail). Identity controls with detail_gain=1 copy the input.

### Current native algorithm/profile

For RGB, derive the Float64 luminance coefficients from the declared RGB basis and form `Y=(R*L0+G*L1)+B*L2`; Gray publishes its selected component as Y. Publish z and the centered, ordered bilateral base with the GRD-19A native algorithm. Evaluate the GRD-19B base exposure and add `(detail_gain-1)*(z-base)` in Float64, omitting the detail term when its coefficient is zero, radius is zero, or the published log and base coordinates compare equal. This equality also defines zero detail for equal infinite coordinates caused by luminance overflow. Form and apply the native gain as in GRD-19B. Complete identity controls and native zero exposure preserve source bits; alpha and unselected components preserve bits independently. The same full-image cropped neighborhood is used for all tile and ROI observations. Radius zero makes the detail term exactly zero, including when finite source RGB overflows the published luminance. Only the base exposure is applied in that case; identity base controls copy for any finite detail gain.

## Behavior

H(radius) through the luminance base plus matching source colors. Alpha/AOVs copy their source bits; bypass-only dependencies have the execution-tile scope specified in the family contract. Effects are applied over the requested output independently of any later effect mask.

## Acceptance

Constant Y has zero detail. detail_gain=1 preserves the source log-detail contribution; detail_gain=0 removes it while applying the selected base exposure. At radius zero, finite extreme RGB that overflows luminance still receives only the base exposure. Relative linear-Y Gray descriptions accept `relative_luminance`.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
