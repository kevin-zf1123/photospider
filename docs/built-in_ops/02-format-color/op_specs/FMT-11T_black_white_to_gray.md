---
spec_schema_version: 1
id: FMT-11T
parent_id: FMT-11
function: black_white_to_gray
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.black_white_to_gray_strict
  - color.black_white_to_gray_fast
  - color.black_white_to_gray_accelerated_apple_silicon
  - color.black_white_to_gray_accelerated_x86_64
---

# FMT-11T: Binary Black/White to explicit Gray levels

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: binary component with declared Gray origin. Target: same Gray kind using chosen black/white values. Shape, channel axis/count and slots are preserved. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. `black_value` and `white_value` are required finite same-dtype constants.

## Mathematics

The selector is exactly 0 or 1 in all modes (a caller precondition); -0 selects black. The selected constant's bits are copied. Equal or reversed levels are legal.

## Behavior

The binary selector is always read, including when both output levels are identical. This member materializes output and rejects forced view, even for a bypass-only request.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

Selector 0.25 or NaN is outside the caller precondition even when both levels equal 0.5; the source selector is still read. Gray l output levels 0/1 do not restore pre-threshold samples. Also cover both dtypes and signed zeros.

## Conceptual DAG

`T -> R -> explicit RGB reconstruction`
