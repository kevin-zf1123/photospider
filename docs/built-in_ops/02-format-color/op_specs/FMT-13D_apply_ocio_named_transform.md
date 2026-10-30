---
spec_schema_version: 1
id: FMT-13D
parent_id: FMT-13
function: apply_ocio_named_transform
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_named_transform_2_5_2_cpu
---

# FMT-13D: configured NamedTransform

Inherits [FMT-13](FMT-13_ocio_transform_contract.md). A NamedTransform is selected as a transform object, not guessed to be a color space.

## Interface

Frozen config/context, an exact NamedTransform name/alias selector and complete entry_space/exit_space declarations for its forward map are required. These endpoint bindings are caller assertions with static structural/unit checks; the object's name, family or description does not prove its colorimetry. direction=forward is default; inverse swaps effective entry/exit and uses the engine's authored inverse or inverse construction. An unsupported object in an older config or an unavailable inverse fails. Nested references and reference bridges obey the family; no source/destination color-space conversion is added from the declared endpoint labels.

One `input` tensor yields same-dtype/shape `values` through the common triple and F32 boundary. Every path supplies explicit endpoint declarations and publishes numeric data without an exit description. Unchecked finite-value premises, structural alpha admission, exact point/group support and materialization apply.

## Acceptance

- A named affine transform with independent expected values in both directions; exact name/alias behavior and rejection through A's ColorSpace namespace.
- Missing endpoints and contradictory explicit endpoint configuration are rejected, as is a config without the selected NamedTransform.
- An alpha-changing named matrix fails admission; ordinary RGB mixing remains allowed. Same-shape output preserves unrelated alpha/AOV bits.
- Nested LUT/context and dynamic property values are frozen; sparse requests, Float64 adaptation and both execution configurations match the point oracle.
