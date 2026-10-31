---
spec_schema_version: 1
id: FMT-11F
parent_id: FMT-11
function: oklab_to_xyz
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.oklab_to_xyz_strict
  - color.oklab_to_xyz_fast
  - color.oklab_to_xyz_accelerated_apple_silicon
  - color.oklab_to_xyz_accelerated_x86_64
---

# FMT-11F: OKLab to relative D65 XYZ

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: OKLab L,a,b. Target: relative D65 XYZ, white Y=1. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: fixed exact inverses; no alternate matrix parameter. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use inverse(M1)*cube(inverse(M2)*OKLab), exact inverses of the selected forward matrices. Retain signed/HDR coordinates and reference.

## Behavior

Sample support: each transformed component uses L,a,b. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

Use exact rational inverse matrices and cubic polynomial evaluation for an independent oracle. Negative L and opposite opponent signs remain valid if requested results are finite. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`F -> optional explicit FMT-10C -> FMT-10B`
