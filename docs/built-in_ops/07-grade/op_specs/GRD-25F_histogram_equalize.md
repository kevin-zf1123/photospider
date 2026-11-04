---
spec_schema_version: 1
id: GRD-25F
parent_id: GRD-25
function: histogram_equalize
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.histogram_equalize
---

# GRD-25F: Complete histogram equalization

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.histogram_equalize` performs the transformation below.

## Interface

`feature: T[H,W]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W]`.

selection_mode, ROI, B and edge are as GRD-25A.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Calculate the histogram, CDF map and application using GRD-25A/B/C equations as `histogram -> histogram_cdf_map -> histogram_apply`. The statistical outputs and map are explicit temporary results reused by the requested application tiles in the same Run. Binary and weighted histograms retain their specified distinction.

## Behavior

W over the metering ROI plus the requested source samples. Selection affects estimation; the mapping applies to every requested output pixel. Effect masking is a separate operation.

## Acceptance

Constant in-domain input is unchanged. A metering ROI smaller than the image still defines the mapping for requested pixels outside it.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
