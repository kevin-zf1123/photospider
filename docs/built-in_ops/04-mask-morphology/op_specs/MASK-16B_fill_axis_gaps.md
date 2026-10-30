---
spec_schema_version: 1
id: MASK-16B
kind: composite_workflow
category: 04-mask-morphology
status: Proposed
document_maturity: D2_draft
implementation_status: implemented_composite_workflow_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-16
function: fill_axis_gaps
oracle_entry: fill_axis_gaps
authoring_api: ps::mask::fill_axis_gaps
authoring_header: photospider/ops/mask/gap_fill.hpp
implemented_template_names:
  - mask.fill_axis_gaps
---

# MASK-16B: Flood using a temporary axis-gap boundary

Inherits the [family contract](MASK-16_close_gap_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Authoring function: `ps::mask::fill_axis_gaps` in `photospider/ops/mask/gap_fill.hpp`; workflow template name `mask.fill_axis_gaps` (not a primitive registry key).

barrier: Binary[H,W], seeds: Binary[H,W] -> fill, barrier, ignored_seeds, reopened_barrier: Binary[H,W]

Static parameters: maximum_gap Int64>=0; fill_connectivity 4|8.

## Mathematics

Filter S=seeds AND NOT original under the unchecked Binary premise. Prepare base closure T, then E=original OR (T AND NOT S); only newly added seed-site barriers are cleared. Empty S produces empty fill. Export E as barrier, seeds AND original as ignored_seeds, and T AND S as reopened_barrier.

A temporary closure, seed-site override, then barrier flood; original-barrier seeds are ignored.

complete-input barrier/seeds; the temporary closure never edits the source barrier.

## Acceptance

a ring with one axial missing cell can become enclosed at gap1; a seed on the missing cell reopens that cell before flood.

All four Binary outputs use the seeds floating dtype. The family fixes workflow export names and diagnostic-only request semantics.

## Conceptual DAG

```text
barrier -> mask.bridge_axis_gaps_strict(maximum_gap) -> T
(barrier, T, seeds) -> mask.prepare_gap_fill_strict -> E (barrier), ignored_seeds, reopened_barrier, S
(S, E, fill_connectivity) -> mask.flood_barrier_strict -> fill
```

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
