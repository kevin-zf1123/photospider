---
spec_schema_version: 1
id: FMT-15C
parent_id: FMT-15
function: compose_view
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
---

# FMT-15C: Explicit view composition

Inherits the [FMT-15 family](FMT-15_tone_view_contract.md), including its normative C section. The public `ps::format::compose_view` helper in `photospider/ops/format/view.hpp` composes native FMT-09/10/14/15 and configured FMT-13A..F stages transactionally. Resource-aware overloads validate OCIO construction, closed resources, direction and canonical endpoints. Native and configured display conditions are explicit. No native C key exists.

## Interface

The compose_view helper takes an input plus a nonempty ordered stage list and a final display assertion, and returns the final values edge. All required per-stage parameters are explicit. The complete intermediate chain is inferred and the real output descriptor returned. No pipeline-wide raw shortcut is supplied.

## Behavior

Each stage's support, rounding, error behavior are those of the constituent. An expanded graph equals a manually authored equivalent graph; invalid transitions leave the graph unchanged. There is no automatic rendering, default screen or reordering. The graph expansion is atomic. No fusion or zero-copy identity shortcut is implied.

## Acceptance

The family's arithmetic fixtures, description/reference conflicts, exact sparse/ROI reads and alpha bit preservation, applied within the family's explicit native/OCIO view DAGs.
