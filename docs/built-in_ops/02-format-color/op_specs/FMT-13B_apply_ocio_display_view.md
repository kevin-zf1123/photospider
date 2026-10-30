---
spec_schema_version: 1
id: FMT-13B
parent_id: FMT-13
function: apply_ocio_display_view
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_display_view_2_5_2_cpu
---

# FMT-13B: configured display and view

Inherits the [FMT-13 family](FMT-13_ocio_transform_contract.md). One engine invocation applies a declared display/view route, with no second FMT-15 tone/view pass.

## Interface

One `input` tensor produces same-dtype/shape `values`. Frozen config, context, exact source_space selector and exact display/view names are required. DisplayViewTransform is constructed in direction=forward by default; inverse consumes the declared view output interpretation and returns the source interpretation. An inverse processor does not recover clipped or rendered data.

looks_bypass=false by default executes the view's configured Looks and admitted compile-time candidates; true skips those Looks. Additional user Looks compose explicitly using C. data_bypass=true by default is independently selectable; semantic isData endpoints remain forbidden. No default display/view is chosen, no monitor is detected, no virtual display is instantiated from the current screen, and viewing-rule menus are not consulted. An unresolved display/view fails.

The selected DisplayViewTransform authorizes its configured view conversion. Additional implicit bridges inside color-space/Look dependencies still obey reference_bridge. Config view transforms, display color-space paths and Looks may all contribute; this is not a display matrix-only operation. The effective route and actual output color space are recorded. View provenance is separate from endpoint units and color-space identity.

## Behavior

One triple, unchanged slots, the F32 engine boundary, finite-only raw, alpha-effect admission and exact point/group demand apply as in the family. Public alpha is never passed to the engine.

## Acceptance

- A scene-space/view/display config with a nontrivial view plus optional Look: enabled/bypassed Looks are verified separately against direct engine results.
- Omitted/missing display/view and semantic data endpoints are rejected. Changing the host monitor or active-display environment does not alter a frozen invocation.
- An explicitly invertible simple view reverses with known expected coordinates; an inverse unavailable to the engine and a lossy view are tested separately, without promising perfect round-trip recovery.
- No additional tone map or FMT-09 transfer is added after the config already produces the desired display encoding. Alpha-only and hidden-color requests, both public dtypes, cross-tile regions and point-oracle agreement under reference/optimized.
