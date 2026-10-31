---
spec_schema_version: 1
id: FMT-11J
parent_id: FMT-11
function: hsl_to_rgb
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.hsl_to_rgb_strict
  - color.hsl_to_rgb_fast
  - color.hsl_to_rgb_accelerated_apple_silicon
  - color.hsl_to_rgb_accelerated_x86_64
---

# FMT-11J: HSL to its described RGB

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: HSL with an explicit underlying relative RGB. Target: the same underlying RGB, linear or encoded. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: source input_hue_unit; explicit in raw. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use exact periodic hue reduction and the six-sector inverse. Do not apply transfer or clamp finite extended S/L.

## Behavior

Sample support: every requested RGB component uses H,S,L, including achromatic input. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

H=0,S=1,L=0.5 gives RGB=(1,0,0). Finite hue is a caller precondition even with S=0. Very large finite multi-turn hue uses exact reduction. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`J -> explicit FMT-09 decode when its restored RGB is encoded`
