---
spec_schema_version: 1
id: FMT-12C
parent_id: FMT-12
function: transform_icc_chain
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.icc_transform_chain_lcms2_19_1_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-12C: execute an ordered ICC profile chain

Inherit the complete [FMT-12 family](FMT-12_icc_transform_contract.md). C is one
native external-engine transform, not a composition of rounded A outputs.
The specification is Proposed and unimplemented.

## Static chain and ports

One `input` tensor produces same-dtype `values`. Supply 2..255 immutable stages
with profile identity, declared direction and applicable intent/BPC. Resolve
whole-chain defaults into per-stage records before constructing the transform.
Ordinary stages use device_to_pcs or pcs_to_device; abstract/DeviceLink stages
use forward. Abstract stages are internal PCS positions only. Check each actual
engine direction and adjacent signature; declared directions are not commands
to invert an unavailable table.

Ordinary/abstract intent defaults to relative_colorimetric, applicable bpc to off, under
the pinned effective rules. DeviceLink instead takes header intent and rejects
user intent/BPC/adaptation overrides. Ordinary device_to_pcs has no BPC field;
its internal API slot is not_applicable. Whole-chain BPC defaults apply only
to actual PCS-connection controls, as specified in the family. Use fixed
adaptation=1.0 where applicable.
No optional acceleration or custom intent plugins are loaded. A chain is not
valid merely because every element can be parsed individually.

The first and last ordinary profiles can establish entry/exit interpretation
when used in corresponding input/output directions. Otherwise require complete
explicit endpoint declarations, as for B. Preserve owned endpoint bindings.
Raw provides all numerical endpoint/slot parameters explicitly without claiming
that the result has a newly validated semantic target.

## Execution and publication

Construct through the pinned extended-transform path using explicit stage
arrays. Record stages, directions, effective settings, actual tags/fallbacks
and inserted PCS bridges. Engine linking/pre-optimization remain observable
parts of this contract; there is no tensor rounding between stages.
execution=optimized is default; reference preserves its own fixed flags.

Public unit conversion occurs only at the chain's endpoints. Apply no extra
project l/L* or CMYK scaling to an internal PCS/DeviceLink connection, which is
already in the engine pipeline convention. Float32 intermediate stages and
engine CLUT paths retain their actual arithmetic even with Float64 ports.

Use family group replacement, planar materialization, point/group support,
finite raw boundary and semantic sample constraints. Alpha/AOVs remain external
to the CMM and bit-preserving. Required construction failures are static even
for bypass-only requests; runtime sample failures have exact requested support.

## Acceptance and conceptual workflow

- A two-ordinary-profile chain with identical settings matches the corresponding
  direct-CMM extended path. Check equivalence with A only where the exact engine
  construction/settings establish it, not by assumption.
- Build source RGB device_to_pcs -> abstract PCS -> destination CMYK pcs_to_device.
  Verify explicit directions and recorded PCS bridges; swapping incompatible
  elements or requesting an unsupported direction fails construction.
- Include an intermediate or endpoint DeviceLink, verifying header intent and
  required explicit endpoint descriptions. Reject abstract endpoints and
  inapplicable per-link controls.
- Fix a chain where one-engine execution differs from separate tensor stages.
  Compare each to its own reference; the compiler must not split C or merge
  separate A nodes into C without proving all observable behavior equivalent.
- Exercise two and 255 stages, checked rejection of 0/1/256, low construction
  work/capacity budgets, latched internal failure and bounded cancellation.

The future public workflow explicitly lists its stages and immutable resources,
binds input metadata, selects an execution configuration and requests a sparse
ROI. No runtime implementation or measured profile corpus exists in this spec.
