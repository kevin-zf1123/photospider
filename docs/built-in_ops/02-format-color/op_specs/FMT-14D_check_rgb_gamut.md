---
spec_schema_version: 1
id: FMT-14D
parent_id: FMT-14
function: check_rgb_gamut
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.check_rgb_gamut_strict
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-14D: RGB gamut diagnostic mask

Inherit the complete [FMT-14 family](FMT-14_gamut_mapping_contract.md), including
its normative D algorithm, interface, metadata, finite-domain raw, errors,
resource obligations and strict numerical reference. Proposed and unimplemented.

## Ports, parameters and output

One `input` Float32/Float64 Value. Output `mask` has the family non-channel shape/keepdims policy and diagnostic metadata.
Select one native relative linear RGB group, or explicit raw ordered slots.
Interpretation defaults to respect, layout to auto; materialize is supported and
view rejected. All metadata/geometry/member parameters are static. No implicit
basis, transfer, range encoding or tone conversion occurs.

## Computation and exact request behavior

One requested mask sample consumes the complete source RGB triple. There is no alpha/AOV output.
Use the corresponding family section for every branch and rounding boundary.
There is no spatial halo or data-dependent demand elision. Static admission
still runs for empty or bypass-only requests. Mapping hidden colors does not read
alpha. Forward dirty support follows the exact source-to-output relation.

## Storage, errors and implementation

Native CPU strict primitive; no acceleration/GPU promise. Inherit planar virtual
reservations, page preparation, transactional region publication, bounded scratch,
NUM resource/cancellation and immutable owner lifetimes. A finite-domain error
has its actual requested observation scope; required upstream failures propagate.
No sample-dependent identity branch makes the whole node eligible for view.

## Independent acceptance and conceptual public workflow

(-0,1,.5) gives +0, (0,1+2^-23,.5) Float32 gives 1. NaN fails instead of being classified inside. Rank-one RGB requires keepdims=true.

Also run the family sparse/ROI, dtype, metadata, resource and lifetime cases.
Conceptual workflow: explicit source decoding/basis conversion -> this member ->
consumer of mapped RGB or diagnostic mask. Implementations must supply an actual
public workflow and independent oracle before marking this spec implemented.
Source comparisons and algorithm compatibility limits are recorded in the family.
