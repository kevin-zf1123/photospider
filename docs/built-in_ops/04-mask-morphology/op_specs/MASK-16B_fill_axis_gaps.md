---
spec_schema_version: 1
id: MASK-16B
kind: composite_workflow
category: 04-mask-morphology
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-16
function: fill_axis_gaps
oracle_entry: fill_axis_gaps
proposed_template_names:
  - mask.fill_axis_gaps
---

# MASK-16B: Flood using a temporary axis-gap boundary

Inherit the complete [family contract](MASK-16_close_gap_contract.md) and
[MASK-common](MASK_common_contract.md). This member is Proposed and unimplemented;
the proposed name is not a current registry entry or a compatibility alias.

## Ports, parameters and output inference

barrier: Binary[H,W], seeds: Binary[H,W] -> fill, barrier, ignored_seeds, reopened_barrier: Binary[H,W]

Required static parameters: maximum_gap Int64>=0; fill_connectivity 4|8. Values and controls use the stated
port order. Infer ordinary output dimensions from descriptors/statics only;
Static Tensor capacities, payload counts and source-association rules, where relevant, are those of the family.
No hidden default, conversion, color/alpha inference or optional port is added.

## Mathematics and exact request behavior

Filter S=seeds AND NOT original under the unchecked Binary premise. Prepare base closure T,
then E=original OR (T AND NOT S); only newly added seed-site barriers are cleared. Empty S produces empty fill. Export E as barrier, seeds AND original as
ignored_seeds, and T AND S as reopened_barrier.

A temporary closure, seed-site override, then barrier flood; original-barrier seeds are ignored.

complete-input barrier/seeds; temporary is immutable scratch, not source edit. Empty Q adds no runtime payload work; static schema checks still apply.
The family defines mathematical Data/Control support, border extension and
rounding stages. Lower the selected complete output tiles to fixed input-tile
reads, using a finite conservative superset for runtime-dependent addressing.
There is no payload Validation read set or runtime dependency discovery.

## Implementation, resource and failure obligations

Use the family reference algorithm and its stated work/scratch bounds. Strict
CPU is the initial target; no implicit accelerated/GPU/third-party fallback is
registered. Exact choices/copies stay exact; arithmetic uses the inherited
rounding boundary and unchecked numerical premises. Account for output, scratch,
exact state, retained source owners and all simultaneous intermediates. Poll
inside long loops/refinement. Selected outputs of one call share its publication
outcome; failure exposes no partial call Result. Already retired call exports
remain valid. Preserve host upstream/cancellation/resource status and immutable
owner lifetime.

## Independent acceptance and conceptual workflow

a ring with one axial missing cell can become enclosed at gap1; a seed on the missing cell reopens that cell before flood.

Independent reference entry: `fill_axis_gaps` in
[`mask16/reference.py`](../../../../oracle/ops/mask_morphology/mask16/reference.py).
Its small-image oracle and fixtures are not a production implementation and
do not exercise kernel registration, resource accounting or compiled tile reads.
Run the [suite commands](../../../../oracle/ops/mask_morphology/README.md),
then add the relevant common fixed-tile support, valid special-value, static
structural-parameter, dtype, correctly paired association, cancellation and
owner-lifetime checks before implementation acceptance. Invalid payload or
numerical-premise fixtures diagnose oracle requests only; they do not require
production rejection.

Conceptual DAG: explicit input preparation -> this member -> an explicit
mask/measurement/image consumer appropriate to the listed output. A real
Compiler/ExecutionContext workflow is still required after registration; this
document does not invent a callable public API. Sources and compatibility
limits are recorded in the family and [research notes](../research-sources.md).

All four Binary outputs use the seeds floating dtype. The family fixes
workflow export names and diagnostic-only request semantics.
