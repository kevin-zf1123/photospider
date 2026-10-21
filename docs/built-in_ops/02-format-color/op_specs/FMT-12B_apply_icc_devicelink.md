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

Inherit the complete [FMT-12 family](FMT-12_icc_transform_contract.md). This
Proposed native external-engine adapter applies one frozen v2/v4 DeviceLink in
the forward direction. It neither creates nor reverses a DeviceLink.

## Interface and interpretation

One `input` tensor produces same-dtype `values`. Static parameters are
link_profile, complete entry_space/exit_space declarations, selected group or
raw slots, mode, execution=optimized/reference, layout and applicable output_axis.
Default execution is optimized. Profile, endpoint and layout obligations inherit
the family. Link signatures must match endpoint models and channel counts.

Fix the engine intent argument to the DeviceLink's valid standard header intent.
Reject additional user intent, BPC or adaptation parameters. The engine may select
the corresponding link tag or its pinned fallback; record what is actually used.
Do not reinterpret this as rebuilding a two-profile transform with new rendering
settings. No proofing or gamut alarm is part of B.

Entry/exit descriptions are explicit caller assertions, not facts proved by
optional sequence tags or channel names. Every path supplies structurally
complete endpoint declarations and resources for formatter units. Static checks
compare those declarations with the link endpoint structure; they do not compare
against labels on the input.

## Output, demand and numerical behavior

Use family unit adaptation, unchecked numerical premises, fixed engine settings and same-count
or changed-count group placement. Retain bypass alpha/AOV bits and remap internal
references. Pass the exit interpretation explicitly to subsequent consumers.
No persistent external alpha reference or multi-owner image storage is introduced.

The complete source color tuple at a point supports any target color request.
No unrequested pixels or alpha are required. Requested target components are published
without sample validation; actual shared engine failures still propagate. Forced
view is invalid even for a mathematically identity link. Resource failures
cannot silently substitute the source data or a different link.

## Acceptance and conceptual workflow

- Apply independently constructed identity and constant DeviceLinks with
  explicit endpoint declarations. Verify exact constant outputs, units and
  formatter effects independently; do not assert a general bitcopy identity.
- Use a link without sequence-description/ID tags. Explicit endpoint declarations
  remain required; missing declarations fail rather than inferred device names.
- Reject wrong model/count, non-link class, unsupported header intent or extra
  BPC/adaptation/intent parameters. Check resource retention by prepared operations, plans and active calls.
- Compare header-selected tag/fallback against a direct-CMM harness. Different explicit entry-profile parameters produce distinct computation
  identities, even when input data and structure are identical.
- Check NaN/Inf and formatter overflow in raw, valid out-of-range finite raw
  CMYK, alpha-only requests and cross-tile exact support.

Public resource helpers, executable operation usage, independent oracle commands
and measured validation coverage are recorded in [implementation, validation and performance](../FMT-12-13-18_implementation.md).
