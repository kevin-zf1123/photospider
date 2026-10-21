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

Inherit [FMT-13](FMT-13_ocio_transform_contract.md). A NamedTransform is selected
as a transform object rather than guessed to be a color space. This native
external-engine adapter is implemented; the decision status remains Proposed. See [implementation, validation and performance](../FMT-12-13-18_implementation.md).

## Interface and meaning

Require frozen config/context, an exact NamedTransform name/alias selector and
complete entry_space/exit_space declarations for its forward map. These endpoint
bindings are caller assertions with static structural/unit checks. The object's
name, family or description does not prove its input/output colorimetry.

direction=forward is default; inverse swaps effective entry/exit and uses the
pinned engine's authored inverse or inverse construction. An unsupported object
in an older config or an unavailable inverse fails. Nested references and
reference bridges obey the family; no source/destination color-space conversion
is silently added merely from the declared endpoint labels.

One `input` tensor yields same-dtype/shape `values` through the common triple and
F32 boundary. Every path supplies explicit endpoint declarations/resources and publishes
numeric data without an attached exit description. All modes apply
unchecked finite-value premises, structural alpha admission, exact point/group
support and materialization.

## Acceptance and conceptual workflow

- Select a named affine transform with independent expected values in both
  directions. Verify exact name/alias behavior and rejection through A's
  ColorSpace namespace.
- Reject missing endpoints and contradictory explicit endpoint configuration. Test resource retention and a config without the
  selected NamedTransform capability/object.
- An alpha-changing named matrix fails admission; ordinary RGB mixing remains
  allowed. Same-shape output preserves unrelated alpha/AOV bits.
- Freeze nested LUT/context and dynamic property values, then compare sparse
  requests, Float64 adaptation and both execution configurations to the point
  oracle.

Public resource helpers, executable operation usage, independent oracle commands
and measured validation coverage are recorded in [implementation, validation and performance](../FMT-12-13-18_implementation.md).
