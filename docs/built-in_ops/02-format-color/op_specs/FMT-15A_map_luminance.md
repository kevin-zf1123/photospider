---
spec_schema_version: 1
id: FMT-15A
parent_id: FMT-15
function: map_luminance
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.map_luminance_v1_strict
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-15A: Explicit-exposure luminance mapping

Inherit the complete [FMT-15 family](FMT-15_tone_view_contract.md), including its
normative A section. Proposed/unimplemented; no old operation alias is restored.

## Interface and inference

Static curve=simple/white_extended, gain=1 default, optional required white for extended, negative_luminance=reject default. Relative linear RGB/linear-Y Gray -> display-relative linear in the same basis.
One input tensor Value produces values with unchanged Float32/Float64 dtype, shape, axis and slots. Select one RGB/linear-Y Gray group; `metadata_mode` defaults to respect and supports call-local override or finite-domain raw. Output metadata follows the family unit/reference rule.

## Formula, request and errors

Full source RGB is consumed for any color output; Gray consumes one sample. Alpha/AOV bypass independently. Neutral 3 with gain=1 and simple gives .75. Negative Y fails unless the explicit signed extension is selected.
The family states every finite domain, rounding boundary and branch. Static
validation remains required for empty/bypass-only observations. Zero alpha does
not remove hidden-color work. Required upstream failures keep their scope.

## Storage and numerical identity

auto/materialize create requested planar values; forced view fails. Native CPU strict arithmetic only in this revision. Account exact arithmetic, scratch, output pages and ancestry; honor cancellation and immutable lifetime.
Metadata and sample-validity guarantees remain separate. No runtime evidence is
claimed by this specification.

## Acceptance and conceptual use

Apply the family's independent arithmetic fixtures, metadata/reference conflicts,
exact sparse/ROI reads, alpha bit preservation, budget/cancellation and lifetime
cases. Use this member within one of the family's explicit native/OCIO view DAGs.
Runtime acceptance requires a real public workflow, exact expected results where
specified, and a separately implemented oracle. Sources and limits are in the
family contract; product-name compatibility is not claimed.
