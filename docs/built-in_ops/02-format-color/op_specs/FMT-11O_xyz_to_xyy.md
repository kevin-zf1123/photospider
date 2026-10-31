---
spec_schema_version: 1
id: FMT-11O
parent_id: FMT-11
function: xyz_to_xyy
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.xyz_to_xyy_strict
  - color.xyz_to_xyy_fast
  - color.xyz_to_xyy_accelerated_apple_silicon
  - color.xyz_to_xyy_accelerated_x86_64
---

# FMT-11O: XYZ to xyY

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: relative or absolute XYZ with explicit white. Target: xyY with unchanged Y units/reference. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: white from input_description; raw requires white_x/white_y. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use exact component sum. Semantic black adopts reference-white xy; nonzero exact cancellation is outside semantic x/y preconditions.

## Behavior

Sample support: x/y use X,Y,Z; output Y copies only source Y. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

XYZ=(1,1,2) gives (0.25,0.25,1). (1,-1,0) is outside semantic x/y preconditions, while a Y-only tile remains evaluable. Raw black produces ratio NaNs. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`FMT-10A -> O -> explicit xyY manipulation`
