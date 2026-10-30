---
spec_schema_version: 1
id: FMT-14B
parent_id: FMT-14
function: reduce_oklab_chroma
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.reduce_oklab_chroma_v1_strict
---

# FMT-14B: OKLab chroma reduction

Inherits the [FMT-14 family](FMT-14_gamut_mapping_contract.md), including its normative B algorithm, interface, finite-domain raw form and errors. The strict CPU operation is the only key.

## Interface

One `input` Float32/Float64 Tensor. Output `values` preserves dtype, shape and RGB slots; alpha and unrelated channels copy unchanged. Select one native relative linear RGB group, or explicit raw ordered slots. Interpretation is explicit static configuration; layout defaults to auto; materialize is supported and view is rejected. All description/geometry/member parameters are static. No basis, transfer, range encoding or tone conversion is implied.

## Mathematics

Every requested RGB component consumes the full source triple and the family's fixed 32/64-step algorithm. The family section gives every branch and rounding boundary. There is no spatial halo or data-dependent demand elision. Static admission runs also for empty or bypass-only requests. Hidden colors are mapped without reading alpha. Finite-domain conditions are unchecked caller premises. No sample-dependent identity branch makes the node eligible for view.

## Acceptance

An in-gamut sample verifies bit identity; an out-of-gamut sample is reconstructed independently through every stored XYZ/OKLab/candidate step. The result passes FMT-14D. No maximum-chroma or minimum-Delta-E claim.

Conceptual workflow: explicit source decoding/basis conversion -> this member -> consumer of mapped RGB.
