---
spec_schema_version: 1
id: FMT-15A
parent_id: FMT-15
function: map_luminance
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.map_luminance_v1_strict
---

# FMT-15A: Explicit-exposure luminance mapping

Inherits the [FMT-15 family](FMT-15_tone_view_contract.md), including its normative A section. The strict CPU operation is the only key.

## Interface

Static curve=simple/white_extended, gain=1 default, white required for extended, negative_luminance=reject default. Relative linear RGB/linear-Y Gray -> display-relative linear in the same basis. One input tensor produces `values` with unchanged Float32/Float64 dtype, shape, axis and slots. One RGB/linear-Y Gray group is selected; its description or the family's explicit raw geometry and unit parameters are supplied. Outputs use the family unit/reference rule without an attached description.

## Behavior

The full source RGB is consumed for any color output; Gray consumes one sample. Alpha/AOV bypass independently. The reject mode has the unchecked premise Y>=0; the explicit signed extension defines the negative-Y domain. Static validation is required for empty/bypass-only observations. Zero alpha does not remove hidden-color work. auto/materialize create independent dense output tiles; forced view fails.

## Acceptance

The family's arithmetic fixtures (neutral 3 with gain=1 and simple gives .75), description/reference conflicts, exact sparse/ROI reads and alpha bit preservation. The member is used within one of the family's explicit native/OCIO view DAGs. Product-name compatibility is not claimed.
