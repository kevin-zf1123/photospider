---
spec_schema_version: 1
id: MASK-16C
kind: composite_workflow
category: 04-mask-morphology
status: Proposed
document_maturity: D2_draft
implementation_status: implemented_composite_workflow_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-16
function: fill_morphological_gaps
oracle_entry: fill_morphological_gaps
authoring_api: ps::mask::fill_morphological_gaps
authoring_header: photospider/ops/mask/gap_fill.hpp
implemented_template_names:
  - mask.fill_morphological_gaps
---

# MASK-16C: Flood using a temporary morphology-closed boundary

Inherits the [family contract](MASK-16_close_gap_contract.md) and [MASK-common](MASK_common_contract.md).

## Interface

Authoring function: `ps::mask::fill_morphological_gaps` in `photospider/ops/mask/gap_fill.hpp`; workflow template name `mask.fill_morphological_gaps` (not a primitive registry key).

barrier: Binary[H,W], seeds: Binary[H,W] -> fill, barrier, ignored_seeds, reopened_barrier: Binary[H,W]

Static parameters: MASK-05 footprint schema; fill_connectivity 4|8.

## Mathematics

Filter S=seeds AND NOT original under the unchecked Binary premise. Prepare base closure T, then E=original OR (T AND NOT S); only newly added seed-site barriers are cleared. Empty S produces empty fill. Export E as barrier, seeds AND original as ignored_seeds, and T AND S as reopened_barrier.

OR original with the full-lattice closing, then MASK-11C; no max-gap guarantee.

complete-input barrier/seeds; pad/stage/crop preparation preserves the family closing.

## Acceptance

radius0 equals barrier flood after filtering original-barrier seeds; original-barrier seeds are ignored, newly added seed-site barriers reopen.

All four Binary outputs use the seeds floating dtype. The family fixes workflow export names and diagnostic-only request semantics.

## Conceptual DAG

```text
barrier -> MASK-06 closing(footprint), OR original barrier -> T
(barrier, T, seeds) -> mask.prepare_gap_fill_strict -> E (barrier), ignored_seeds, reopened_barrier, S
(S, E, fill_connectivity) -> mask.flood_barrier_strict -> fill
```

## References

Sources and compatibility limits are in the family contract and the [research notes](../research-sources.md).
