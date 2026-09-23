---
spec_schema_version: 1
id: FMT-13A
parent_id: FMT-13
function: convert_ocio_space
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.ocio_convert_space_2_5_2_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-13A: configured color-space conversion

Inherit the complete [FMT-13 family](FMT-13_ocio_transform_contract.md).
This native OCIO v2.5.2 CPU adapter is Proposed/unimplemented.

## Interface and behavior

One `input` tensor yields same-dtype/shape `values`. Explicit config/context
resources and source_space/destination_space describe the forward route. Public
selectors are exact name/alias/role selectors resolved to canonical ColorSpace
objects; NamedTransform is invalid here. Select one ordered three-component group
or explicit raw slots. All common direction, execution, metadata, domain, resource,
layout and request rules apply. No transfer or unit conversion is inserted.

Construct a ColorSpaceTransform with direction=forward by default; inverse swaps
the effective endpoint interpretation. data_bypass defaults true. Respect/override
reject isData entry/exit spaces; raw may use them. Keep recorded same-name and
equalitygroup skips, including their Float64 narrowing boundary. reference_bridge
defaults reject; config_default explicitly admits and records the configured
scene/display bridge. Neither OPTIMIZATION_NONE nor no-op removes those checks.

Semantic output publishes the effective destination's config/context identity
and canonical space or its explicit compatible binding. Source and target names
alone do not establish native RGB primaries, normalized units or an ACES version.
Copy alpha/AOVs outside the processor. Any requested transformed component
consumes all three source components at the same point, including no-op cases.

## Acceptance and conceptual workflow

- A self-contained two-space config with an independently specified matrix
  checks source/destination resolution, noncanonical slots and exact simple
  coordinates. Compare both execution configurations to their point oracles.
- Same-space Float64 `1+2^-30` produces `1` after RN32, not the original bit
  pattern. Float32 no-op still validates all three consumed components and
  does not allow view.
- Exact alias and role references resolve identically to their canonical name;
  case mismatch or selecting a NamedTransform through A fails statically.
- Scene/display crossing fails by default and records a specific bridge when
  config_default is explicit, even if the config has no named default and the
  engine selects its first eligible view transform.
- isData is rejected in semantic mode and exercised with both data_bypass values
  in raw. NaN in a source peer fails a requested color; NaN in bypass alpha does
  not. Sparse/batched observations preserve the normative point result.

A future public workflow imports frozen config/resources, binds its source
interpretation, selects A and requests an offset ROI. This is conceptual;
current runtime registration and executed numerical evidence are not claimed.
