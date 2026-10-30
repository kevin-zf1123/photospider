---
spec_schema_version: 1
id: FMT-11P
parent_id: FMT-11
function: xyy_to_xyz
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.xyy_to_xyz_strict
  - color.xyy_to_xyz_accelerated_apple_silicon
  - color.xyy_to_xyz_accelerated_x86_64
---

# FMT-11P: xyY to XYZ

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: relative or absolute xyY. Target: XYZ with unchanged Y units/reference. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: no implicit target white, normalization or epsilon. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use X=xY/y and Z=(1-x-y)Y/y. Nonzero y is a caller precondition for semantic X/Z even at Y=0.

## Behavior

Sample support: X/Z use all x,y,Y; output Y copies only source Y. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Acceptance

(0.25,0.25,1) gives (1,1,2). (0.5,0,0) is outside semantic X/Z preconditions, while a Y-only tile returns zero. Arbitrary nonblack Y=0 XYZ need not round-trip. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`P -> FMT-10B with explicit white policy`
