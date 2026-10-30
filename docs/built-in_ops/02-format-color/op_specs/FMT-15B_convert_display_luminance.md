---
spec_schema_version: 1
id: FMT-15B
parent_id: FMT-15
function: convert_display_luminance
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.convert_display_luminance_strict
---

# FMT-15B: Display relative and nits conversion

Inherits the [FMT-15 family](FMT-15_tone_view_contract.md), including its normative B section. The strict CPU operation is the only key.

## Interface

Static direction is mandatory; black_nits/white_nits are mandatory with 0<=black<white. Source units and display reference are matched, and the actual destination unit/range convention is published. One input tensor produces `values` with unchanged Float32/Float64 dtype, shape, axis and slots. One RGB/linear-Y Gray group is selected; its description or the family's explicit raw geometry and unit parameters are supplied. Outputs use the family unit/reference rule without an attached description.

## Behavior

Each output color component consumes only itself. There is no clip, tone curve or implicit peak; finite representable requested results are an unchecked caller premise. Static validation is required for empty/bypass-only observations. Zero alpha does not remove hidden-color work. auto/materialize create independent dense output tiles; forced view fails.

## Acceptance

black=1, white=101 maps .5 to 51 nits. Description/reference conflicts, exact sparse/ROI reads and alpha bit preservation. The member is used within one of the family's explicit native/OCIO view DAGs.
