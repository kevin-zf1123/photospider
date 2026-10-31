---
spec_schema_version: 1
id: GRD-06B
parent_id: GRD-06
function: gradient_map_image
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
registered_operation_keys:
  - grade.gradient_map_image
---

# GRD-06B: Image gradient mapping

Inherits the [family contract](GRD-06_gradient_map_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.gradient_map_image` performs the transformation below.

## Interface

`input: T[S,C]`, `knots: Float64[K]`, `colors: Float32/64[K,3]` -> `values: T[S,C]`.

Select a complete RGB group. Required feature_source=linear_y|cielab_l|oklab_l|max_rgb and destination RGB description. Ramp edge and K are as GRD-06A. RGB conversion uses the common color routes.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Extract the named feature from the original RGB group, evaluate GRD-06A and replace the selected three slots by the ramp RGB. max_rgb is the maximum of the declared original RGB coordinates. Other feature routes operate on relative linear RGB. Preserve all other slots.

## Current native implementation profile

`grade.gradient_map_image` accepts raw Int64 `axis` with a comma-separated `components` String, or canonical `input_description` with a `group` String. `feature_source` and destination `color_description` are required Strings. For the three brightness routes, `source_basis` may explicitly supply the RGB basis; otherwise the selected semantic description must resolve it. `edge` is `reject` or `clamp`, default `clamp`. `max_work` is a nonnegative Int64, default 9223372036854775807.

Feature calculation and shared three-column ramp evaluation use binary64. `linear_y` uses the declared RGB-to-XYZ Y row. `cielab_l` evaluates the normalized CIE L branch from relative Y. `oklab_l` adapts the source basis to D65, forms LMS, applies signed cube roots and the native OKLab L row. The x86 span uses a cube-root seed followed by two Halley refinements; exceptional lanes use the scalar library path. ARM spans retain native scalar cube roots with vector matrix/ramp work. Unselected slots are copied as raw bits, including alpha/AOV payloads.

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

For a constant source feature, every selected output pixel has the same ramp color. Alpha and AOV slots retain their original positions and bits.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
