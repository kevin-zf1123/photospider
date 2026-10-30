---
spec_schema_version: 1
id: FMT-14A
parent_id: FMT-14
function: clip_rgb_gamut
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.clip_rgb_gamut_strict
---

# FMT-14A: RGB component gamut clipping

Inherits the [FMT-14 family](FMT-14_gamut_mapping_contract.md), including its normative A algorithm, interface, finite-domain raw form and errors. The strict CPU operation is the only key.

## Interface

One `input` Float32/Float64 Tensor. Output `values` preserves dtype, shape and RGB slots; alpha and unrelated channels copy unchanged. Select one native relative linear RGB group, or explicit raw ordered slots. Interpretation is explicit static configuration; layout defaults to auto; materialize is supported and view is rejected. All description/geometry/member parameters are static. No basis, transfer, range encoding or tone conversion is implied.

## Mathematics

Each requested RGB component reads only its corresponding source sample. The family section gives every branch and rounding boundary. There is no spatial halo or data-dependent demand elision. Static admission runs also for empty or bypass-only requests. Hidden colors are mapped without reading alpha. Finite-domain conditions are unchecked caller premises. No sample-dependent identity branch makes the node eligible for view.

## Acceptance

For R=-0.25 and G=NaN, an output containing only R returns +0. An output that consumes G violates the finite caller premise and has no required diagnostic. Clipping can alter hue and luminance. Also cover sparse/ROI, dtype and description cases.

Conceptual workflow: explicit source decoding/basis conversion -> this member -> consumer of mapped RGB.
