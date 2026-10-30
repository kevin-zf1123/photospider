---
spec_schema_version: 1
id: GRD-24I
parent_id: GRD-24
function: auto_levels_channels
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_levels_channels
---

# GRD-24I: Automatic per-channel levels

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_levels_channels` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Explicit selected numeric/RGB components. Selection/alpha/ROI as GRD-24; probabilities default [0.01,0.99], gamma=1,out0=0,out1=1, with the domains of GRD-24E.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Compute quantiles and GRD-24C controls independently for each selected component. Apply GRD-04A to each component with state=0; copy it when state=1. All statistics use the original image.

## Behavior

W over the selected components in the metering ROI plus the requested source samples. Bypass components and alpha remain independent.

## Acceptance

A constant red channel copies while a varying green channel is adjusted. This mode can change RGB ratios independently of brightness-only paths.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
