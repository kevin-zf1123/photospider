---
spec_schema_version: 1
id: FMT-12B
parent_id: FMT-12
function: apply_icc_devicelink
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.icc_apply_devicelink_lcms2_19_1_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
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
optional sequence tags or channel names. Respect verifies the selected input
interpretation against entry_space; override explicitly replaces it locally.
Raw still supplies structurally complete endpoint declarations and resources
needed to define formatter units, but bypasses semantic consistency and does
not publish exit_space as a new valid color declaration.

## Output, demand and numerical behavior

Use family unit adaptation, domain checks, fixed engine settings and same-count
or changed-count group placement. Retain bypass alpha/AOV bits and remap internal
references. Publish complete declared exit interpretation only in semantic mode.
No persistent external alpha reference or multi-owner image storage is introduced.

The complete source color tuple at a point supports any target color request.
No unrequested pixels or alpha are required. Only requested target components
receive sample validity checks; shared engine failures still propagate. Forced
view is invalid even for a mathematically identity link. Resource failures
cannot silently substitute the source data or a different link.

## Acceptance and conceptual workflow

- Apply independently constructed identity and constant DeviceLinks with
  explicit endpoint declarations. Verify exact constant outputs, units and
  formatter effects independently; do not assert a general bitcopy identity.
- Use a link without sequence-description/ID tags. Explicit endpoint declarations
  remain required; missing declarations fail rather than inferred device names.
- Reject wrong model/count, non-link class, unsupported header intent or extra
  BPC/adaptation/intent parameters. Check that output profile resources remain
  alive after the execution context is destroyed.
- Compare header-selected tag/fallback against a direct-CMM harness. A tagged
  CMYK input using a different declared entry profile fails in respect; explicit
  override permits intentional reinterpretation without mutating other consumers.
- Check NaN/Inf and formatter overflow in raw, valid out-of-range finite raw
  CMYK, alpha-only requests and cross-tile exact support.

A future public graph imports a CMYK-to-CMYK DeviceLink and its explicit printing
condition declarations, binds planar CMYK plus alpha, requests selected target
inks and verifies fixed outputs. This is a conceptual graph, not a registered
runtime example or a claim that the old CMYK resource importer accepts links.
