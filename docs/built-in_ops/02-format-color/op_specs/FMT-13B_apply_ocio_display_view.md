---
spec_schema_version: 1
id: FMT-13B
parent_id: FMT-13
function: apply_ocio_display_view
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_display_view_2_5_2_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13B: configured display and view

Inherit the complete [FMT-13 family](FMT-13_ocio_transform_contract.md).
One native engine invocation applies a declared display/view route, with no
implicit second FMT-15 tone/view pass. Proposed/unimplemented.

## Interface and selected route

One `input` tensor produces same-dtype/shape `values`. Require frozen config,
context, exact source_space selector and exact display/view names. Construct
DisplayViewTransform in direction=forward by default; inverse consumes the
declared view output interpretation and returns the source interpretation.
The existence of an inverse processor does not recover clipped or rendered data.

looks_bypass=false by default executes the view's configured Looks and admitted
compile-time candidates; true skips those Looks and participates in identity.
Additional user Looks compose explicitly using C. data_bypass=true by default
is independently selectable; semantic isData endpoints remain forbidden.
Do not choose a default display/view, invoke monitor detection, instantiate a
virtual display from the current screen, or use viewing-rule menus to guess
the requested route. An unresolved display/view fails.

The selected DisplayViewTransform explicitly authorizes its configured view
conversion. Additional implicit bridges inside color-space/Look dependencies
still obey reference_bridge. Config view transforms, display color-space paths
and Looks may all contribute; this is not a display matrix-only operation.
Record the effective route and actual output color space. Keep view provenance
separate from endpoint units and color-space identity.

## Samples, demand and acceptance

Inherit one triple, unchanged slots, F32 engine boundary, finite-only raw,
alpha-effect admission, exact point/group demand, materialization and all host
integration prerequisites. Public alpha is never passed to the engine.

- Fix a scene-space/view/display config with a nontrivial view plus optional
  Look. Verify enabled/bypassed Looks separately against direct engine results.
- Reject omitted/missing display/view and semantic data endpoints. Changing the
  host monitor or active-display environment does not alter a frozen invocation.
- Reverse an explicitly invertible simple view with known expected coordinates;
  separately test an inverse unavailable to the engine and a lossy view without
  promising perfect round-trip recovery.
- Ensure no additional project tone map or FMT-09 transfer is added after the
  config already produces the desired display encoding. Verify alpha-only and
  hidden-color requests, both public dtypes, cross-tile regions and point-oracle
  agreement under reference/optimized.

A future public workflow binds a specific versioned ACES config, source space,
display and view, then requests rendered RGB and alpha separately. Actual
runtime commands and measured outputs are implementation acceptance work.
