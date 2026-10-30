---
spec_schema_version: 1
id: FMT-12B
parent_id: FMT-12
function: apply_icc_devicelink
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.icc_apply_devicelink_lcms2_19_1_cpu
---

# FMT-12B: apply an ICC DeviceLink

Inherits the [FMT-12 family](FMT-12_icc_transform_contract.md). This external-engine adapter applies one frozen v2/v4 DeviceLink in the forward direction. It neither creates nor reverses a DeviceLink.

## Interface

One `input` tensor produces same-dtype `values`. Static parameters are link_profile, complete entry_space/exit_space declarations, selected group or raw slots, mode, execution=optimized/reference, layout and applicable output_axis. Default execution is optimized. Profile, endpoint and layout obligations follow the family. Link signatures match endpoint models and channel counts.

The engine intent argument is fixed to the DeviceLink's valid standard header intent. Additional user intent, BPC or adaptation parameters are rejected. The engine may select the corresponding link tag or its pinned fallback. This is not a rebuild of a two-profile transform with new rendering settings. No proofing or gamut alarm is part of B.

Entry/exit descriptions are explicit caller assertions, not facts proved by optional sequence tags or channel names. Every path supplies structurally complete endpoint declarations and resources for formatter units. Static checks compare those declarations with the link endpoint structure.

## Behavior

Family unit adaptation, unchecked numerical premises, fixed engine settings and same-count or changed-count group placement apply. Bypass alpha/AOV bits are retained and internal references remapped. The exit interpretation is passed explicitly to subsequent consumers.

The complete source color tuple at a point supports any target color request; no unrequested pixels or alpha are required. Forced view is invalid even for a mathematically identity link. Resource failures never substitute the source data or a different link.

## Acceptance

- Independently constructed identity and constant DeviceLinks with explicit endpoint declarations: exact constant outputs, units and formatter effects. A general bit-copy identity is not asserted.
- A link without sequence-description/ID tags: explicit endpoint declarations remain required; missing declarations fail rather than inferring device names.
- Wrong model/count, non-link class, unsupported header intent or extra BPC/adaptation/intent parameters are rejected.
- Header-selected tag/fallback matches a direct-CMM harness. Different explicit entry-profile parameters produce distinct results even when input data and structure are identical.
- Raw fixtures satisfying the finite input, unit-adapter, pipeline and output premises, including valid out-of-range finite CMYK; alpha-only requests and raw bypass bits.
