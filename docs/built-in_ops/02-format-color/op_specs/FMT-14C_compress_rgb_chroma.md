---
spec_schema_version: 1
id: FMT-14C
parent_id: FMT-14
function: compress_rgb_chroma
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.compress_rgb_chroma_v1_strict
---

# FMT-14C: Linear RGB neutral-ray compression

Inherits the [FMT-14 family](FMT-14_gamut_mapping_contract.md), including its normative C algorithm, interface, finite-domain raw form and errors. The strict CPU operation is the only key.

## Interface

One `input` Float32/Float64 Tensor. Output `values` preserves dtype, shape and RGB slots; alpha and unrelated channels copy unchanged. Static knee defaults to RN64(0.8); exact positive Y weights are mandatory. Select one native relative linear RGB group, or explicit raw ordered slots. Interpretation is explicit static configuration; layout defaults to auto; materialize is supported and view is rejected. All description/geometry/member parameters are static. No basis, transfer, range encoding or tone conversion is implied.

## Mathematics

Every requested RGB component consumes the full triple. The family section gives every branch and rounding boundary. There is no spatial halo or data-dependent demand elision. Static admission runs also for empty or bypass-only requests. Hidden colors are mapped without reading alpha. Finite-domain conditions are unchecked caller premises. No sample-dependent identity branch makes the node eligible for view.

## Acceptance

With the family dyadic basis, x=(1.5,.25,0) and knee=.5 produces (.9375,.390625,.28125). In-gamut near-boundary changes and non-idempotence are tested.

Conceptual workflow: explicit source decoding/basis conversion -> this member -> consumer of mapped RGB.
