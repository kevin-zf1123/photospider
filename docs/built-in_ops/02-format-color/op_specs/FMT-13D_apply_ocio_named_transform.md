---
spec_schema_version: 1
id: FMT-13D
parent_id: FMT-13
function: apply_ocio_named_transform
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_named_transform_2_5_2_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13D: configured NamedTransform

Inherit [FMT-13](FMT-13_ocio_transform_contract.md). A NamedTransform is selected
as a transform object rather than guessed to be a color space. This native
external-engine adapter is Proposed/unimplemented.

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
F32 boundary. Raw still supplies explicit endpoint declarations/resources but
does not publish exit_space as a newly valid semantic target. All modes apply
finite checks, alpha admission, exact point/group support and materialization.

## Acceptance and conceptual workflow

- Select a named affine transform with independent expected values in both
  directions. Verify exact name/alias behavior and rejection through A's
  ColorSpace namespace.
- Reject missing endpoints and source metadata conflicts; explicit override
  affects only this call. Test resource retention and a config without the
  selected NamedTransform capability/object.
- An alpha-changing named matrix fails admission; ordinary RGB mixing remains
  allowed. Same-shape output preserves unrelated alpha/AOV bits.
- Freeze nested LUT/context and dynamic property values, then compare sparse
  requests, Float64 adaptation and both execution configurations to the point
  oracle.

A future public graph imports the named object and explicit endpoint bindings,
then selects D. No executable registry support is claimed yet.
