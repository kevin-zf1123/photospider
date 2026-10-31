---
spec_schema_version: 1
id: FMT-11E
parent_id: FMT-11
function: xyz_to_oklab
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.xyz_to_oklab_strict
  - color.xyz_to_oklab_fast
  - color.xyz_to_oklab_accelerated_apple_silicon
  - color.xyz_to_oklab_accelerated_x86_64
---

# FMT-11E: Relative D65 XYZ to OKLab

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: relative D65 XYZ, white Y=1. Target: OKLab L,a,b. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: fixed versioned matrices; no alternate matrix/white parameter. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use the exact pinned M2*cbrt(M1*XYZ), with real signed cube roots and only final output rounding. No clipping or white correction.

## Behavior

Sample support: each transformed component uses X,Y,Z. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

Black maps to black. Verify the fixed matrix coefficients against the pinned source and evaluate D65 white residual with an independent high-precision oracle; do not force a=b=0. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`FMT-10A -> explicit D65 adaptation if necessary -> E`
