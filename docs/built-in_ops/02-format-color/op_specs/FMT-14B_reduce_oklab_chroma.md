---
spec_schema_version: 1
id: FMT-14B
parent_id: FMT-14
function: reduce_oklab_chroma
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.reduce_oklab_chroma_v1_strict
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-14B: OKLab chroma reduction

Inherit the complete [FMT-14 family](FMT-14_gamut_mapping_contract.md), including
its normative B algorithm, interface, metadata, finite-domain raw, errors,
resource obligations and strict numerical reference. Proposed and unimplemented.

## Ports, parameters and output

One `input` Float32/Float64 Value. Output `values` preserves dtype, shape and RGB slots; alpha and unrelated channels copy unchanged.
Select one native relative linear RGB group, or explicit raw ordered slots.
Interpretation defaults to respect, layout to auto; materialize is supported and
view rejected. All metadata/geometry/member parameters are static. No implicit
basis, transfer, range encoding or tone conversion occurs.

## Computation and exact request behavior

Every requested RGB component consumes the full source triple and the family fixed 32/64-step algorithm.
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

Use an in-gamut sample to verify bit identity, then an out-of-gamut sample and independently reconstruct every stored XYZ/OKLab/candidate step. The result passes FMT-14D. No maximum-chroma or minimum-Delta-E claim.

Also run the family sparse/ROI, dtype, metadata, resource and lifetime cases.
Conceptual workflow: explicit source decoding/basis conversion -> this member ->
consumer of mapped RGB or diagnostic mask. Implementations must supply an actual
public workflow and independent oracle before marking this spec implemented.
Source comparisons and algorithm compatibility limits are recorded in the family.
