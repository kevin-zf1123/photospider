---
spec_schema_version: 1
id: FMT-11N
parent_id: FMT-11
function: ycbcr_ncl_to_rgb
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.ycbcr_ncl_to_rgb_strict
  - color.ycbcr_ncl_to_rgb_fast
  - color.ycbcr_ncl_to_rgb_accelerated_apple_silicon
  - color.ycbcr_ncl_to_rgb_accelerated_x86_64
---

# FMT-11N: NCL YCbCr to encoded RGB

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: native full-resolution Y′,Cb,Cr. Target: its described transfer-encoded RGB. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: ncl_matrix from source; explicit in raw. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use exact expanded inverse rows without separately rounded R/B intermediates. Restore encoded RGB; do not decode its transfer.

## Behavior

Sample support: R′ uses Y′,Cr; B′ uses Y′,Cb; G′ uses all three. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Native fast numerical profile

The additional `_fast` operation key uses the same mathematical transform with the native FP32/FP64 algorithm and profile defined in the [family contract](FMT-11_model_conversion_contract.md#native-fast-numerical-profile). The strict and accelerated keys retain their existing numerical contracts.

## Acceptance

Input (Y′,0,0) reconstructs equal RGB. NaN Cb is outside semantic G/B preconditions; a R-only tile does not consume it. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`codec -> FMT-06 native decoding -> N -> explicit FMT-09 decode`
