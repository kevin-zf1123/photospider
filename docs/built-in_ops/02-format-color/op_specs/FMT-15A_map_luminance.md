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

Inherit the complete [FMT-15 family](FMT-15_tone_view_contract.md), including its
normative A section. The specification remains Proposed; the strict CPU operation is registered with fixed invocation ABI 8. See [implementation and validation](../FMT-14-15_implementation.md). No old operation alias is restored.

## Interface and inference

Static curve=simple/white_extended, gain=1 default, optional required white for extended, negative_luminance=reject default. Relative linear RGB/linear-Y Gray -> display-relative linear in the same basis.
One input tensor Tensor produces values with unchanged Float32/Float64 dtype, shape, axis and slots. Select one RGB/linear-Y Gray group; supply its description or the family's explicit raw geometry and unit parameters. Outputs use the family unit/reference rule without attaching a description.

## Formula, request and errors

Full source RGB is consumed for any color output; Gray consumes one sample. Alpha/AOV bypass independently. Neutral 3 with gain=1 and simple gives .75. The reject mode has the unchecked premise Y>=0; the explicit signed extension defines the negative-Y domain.
The family states every finite domain, rounding boundary and branch. Static
validation remains required for empty/bypass-only observations. Zero alpha does
not remove hidden-color work. Required upstream failures keep their scope.

## Storage and numerical identity

auto/materialize create independent dense output tiles; forced view fails. Native CPU strict arithmetic only in this revision. Admit exact arithmetic scratch, output allocations and retained owners before issue; honor cancellation, call-retirement publication and immutable lifetime.
Metadata and sample-validity guarantees remain separate. Runtime evidence and its limits are recorded in the linked implementation page.

## Acceptance and conceptual use

Apply the family's independent arithmetic fixtures, metadata/reference conflicts,
exact sparse/ROI reads, alpha bit preservation, budget/cancellation and lifetime
cases. Use this member within one of the family's explicit native/OCIO view DAGs.
Runtime acceptance requires a real public workflow, exact expected results where
specified, and a separately implemented oracle. Sources and limits are in the
family contract; product-name compatibility is not claimed.
