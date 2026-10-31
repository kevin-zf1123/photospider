---
spec_schema_version: 1
id: FMT-11I
parent_id: FMT-11
function: rgb_to_hsl
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.rgb_to_hsl_strict
  - color.rgb_to_hsl_fast
  - color.rgb_to_hsl_accelerated_apple_silicon
  - color.rgb_to_hsl_accelerated_x86_64
---

# FMT-11I: Described RGB to HSL

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: relative described linear or encoded RGB. Target: HSL with the same underlying RGB interpretation. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: required output_hue_unit. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use exact extrema, six-sector hue and the chosen algebraic saturation extension. Gray produces H=S=+0; finite HDR/negative data is not clipped.

## Behavior

Sample support: each H/S/L request uses R,G,B; only S evaluates its singular denominator. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

RGB=(1,0,0) gives (0,1,0.5). RGB=(1,0,-1) is outside the semantic S precondition; a tile containing only H/L can still be evaluated. Retain the actual underlying transfer. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`I -> explicit HSL adjustment -> J`
