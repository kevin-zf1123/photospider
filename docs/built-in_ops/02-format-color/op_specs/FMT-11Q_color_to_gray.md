---
spec_schema_version: 1
id: FMT-11Q
parent_id: FMT-11
function: color_to_gray
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.color_to_gray_strict
  - color.color_to_gray_accelerated_apple_silicon
  - color.color_to_gray_accelerated_x86_64
---

# FMT-11Q: Described color to corresponding Gray

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: XYZ, NCL YCbCr, normalized CIELAB or OKLab. Target: linear_y, encoded_luma, cielab_l or oklab_l Gray. The Q/R structural remapping and axis rules of the family apply. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. gray_kind is inferred semantically and explicit in raw.

## Mathematics

The native coordinate (Y, Y', l or L) is copied; the caller supplies the corresponding Gray configuration downstream. The three selected slots contract into one at their smallest physical index; the channel axis and other channels are retained.

## Behavior

Only the selected Y/Y'/l/L is consumed mathematically; discarded chroma is not read. Auto/materialize copy into independent dense output tiles. A nonempty forced view fails `InvalidArgument/ViewUnavailable`. Empty has no payload. Semantic sample validity is a caller precondition.

## Acceptance

Lab=(0.5,NaN,10) permits Gray l=0.5. Permuted/interleaved logical group indices and alpha/AOV remapping are tested independently of the physical layout. Also cover mixed component requests, both dtypes and signed zeros.

## Conceptual DAG

`linear RGB -> FMT-10A -> Q` for luminance Gray; `relative XYZ -> A or E -> Q` for the corresponding lightness Gray (E requires D65); `encoded RGB -> M -> Q` for luma Gray.
