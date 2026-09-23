---
spec_schema_version: 1
id: FMT-15C
parent_id: FMT-15
function: compose_view
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-15C: Explicit view composition

Inherit the complete [FMT-15 family](FMT-15_tone_view_contract.md), including its
normative C section. Proposed/unimplemented; no old operation alias is restored.

## Interface and inference

The compose_view helper takes input plus a nonempty ordered stage list and final display assertion. It returns the final values edge; all required per-stage parameters are explicit.
This is an authoring interface, not a native registration. Infer the complete intermediate chain and return its real output descriptor. No pipeline-wide raw shortcut is supplied.

## Formula, request and errors

Actual stage support, rounding, error and resource behavior is preserved. Compare with a manually authored equivalent graph; invalid transitions leave the graph unchanged. No automatic rendering, default screen or reordering.
The family states every finite domain, rounding boundary and branch. Static
validation remains required for empty/bypass-only observations. Zero alpha does
not remove hidden-color work. Required upstream failures keep their scope.

## Storage and numerical identity

Every generated node keeps its own numerical profile, planar owner, scratch and engine-resource accounting. Stage the graph expansion atomically. No fusion or zero-copy identity shortcut is implied.
Metadata and sample-validity guarantees remain separate. No runtime evidence is
claimed by this specification.

## Acceptance and conceptual use

Apply the family's independent arithmetic fixtures, metadata/reference conflicts,
exact sparse/ROI reads, alpha bit preservation, budget/cancellation and lifetime
cases. Use this member within one of the family's explicit native/OCIO view DAGs.
Runtime acceptance requires a real public workflow, exact expected results where
specified, and a separately implemented oracle. Sources and limits are in the
family contract; product-name compatibility is not claimed.
