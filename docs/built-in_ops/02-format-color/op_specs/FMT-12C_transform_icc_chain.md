---
spec_schema_version: 1
id: FMT-12C
parent_id: FMT-12
function: transform_icc_chain
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.icc_transform_chain_lcms2_19_1_cpu
---

# FMT-12C: execute an ordered ICC profile chain

Inherits the [FMT-12 family](FMT-12_icc_transform_contract.md). C is one external-engine transform, not a composition of rounded A outputs.

## Interface

One `input` tensor produces same-dtype `values`. 2..255 immutable stages are supplied, each with profile identity, declared direction and applicable intent/BPC. Whole-chain defaults resolve into per-stage records before the transform is constructed. Ordinary stages use device_to_pcs or pcs_to_device; abstract/DeviceLink stages use forward. Abstract stages are internal PCS positions only. Each actual engine direction and adjacent signature is checked; declared directions do not command inversion of an unavailable table.

Ordinary/abstract intent defaults to relative_colorimetric, applicable bpc to off, under the pinned effective rules. DeviceLink takes its header intent and rejects user intent/BPC/adaptation overrides. Ordinary device_to_pcs has no BPC field; its effective BPC is not_applicable. Whole-chain BPC defaults apply only to actual PCS-connection controls, as in the family. Adaptation is fixed at 1.0 where applicable. No acceleration or custom intent plugins are loaded. A chain is not valid merely because every element parses individually.

The first and last ordinary profiles establish entry/exit interpretation when used in corresponding input/output directions. Otherwise complete explicit endpoint declarations are required, as for B. Raw provides all numerical endpoint/slot parameters explicitly without claiming a newly validated semantic target.

The public icc_chain authoring helper seals the ordered stage list and bytes. It performs no CMM construction; the operation performs full validation and construction.

## Behavior

The transform is constructed through the pinned extended-transform path using explicit stage arrays. Stages, directions, effective settings, actual tags/fallbacks and inserted PCS bridges are recorded. Engine linking/pre-optimization are part of this contract; there is no tensor rounding between stages. execution=optimized is default; reference uses its own fixed flags.

Public unit conversion occurs only at the chain's endpoints. No project l/L* or CMYK scaling applies to an internal PCS/DeviceLink connection, which uses the engine pipeline convention. Float32 intermediate stages and engine CLUT paths retain their actual arithmetic even with Float64 ports.

Group replacement, planar materialization, point/group support, finite raw boundary and semantic sample constraints follow the family. Alpha/AOVs remain external to the CMM and bit-preserving. Construction failures are static even for bypass-only requests.

## Acceptance

- A two-ordinary-profile chain with identical settings matches the corresponding direct-CMM extended path. Equivalence with A holds only where the exact engine construction/settings establish it.
- Source RGB device_to_pcs -> abstract PCS -> destination CMYK pcs_to_device: explicit directions and recorded PCS bridges; swapping incompatible elements or requesting an unsupported direction fails construction.
- An intermediate or endpoint DeviceLink: header intent and required explicit endpoint descriptions. Abstract endpoints and inapplicable per-link controls are rejected.
- A chain where one-engine execution differs from separate tensor stages; each is compared to its own reference. C is never split, nor separate A nodes merged into C, without proving equivalent observable behavior.
- Two and 255 stages; 0/1/256 are rejected.
