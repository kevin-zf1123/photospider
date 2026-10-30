---
spec_schema_version: 1
id: FMT-11R
parent_id: FMT-11
function: gray_to_color
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.gray_to_color_strict
  - color.gray_to_color_accelerated_apple_silicon
  - color.gray_to_color_accelerated_x86_64
---

# FMT-11R: Gray to corresponding neutral color

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: one of the four native Gray kinds. Target: neutral XYZ, NCL YCbCr, normalized CIELAB or OKLab. The Q/R structural remapping and axis rules of the family apply. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Gray kind and white come from input_description; raw requires explicit kind and linear-Y white; output_axis is required for axis-free input.

## Mathematics

The Gray slot expands into three canonical-order slots, retaining other channels. Native linear-Y gives Y*W; other kinds give the coordinate plus two +0 opponents. No original chroma is recovered.

## Behavior

Outputs that depend on Gray read it; inserted zero opponents have no sample dependency. This member materializes output and rejects forced view, even for a bypass-only request. Semantic sample validity is a caller precondition.

## Acceptance

Gray Y=0.5 with white xy=(0.25,0.25) yields XYZ=(0.5,0.5,1). A request for only Lab a*/b* constants does not read the source l. Also cover mixed component requests, both dtypes and signed zeros.

## Conceptual DAG

`R -> B/F/N or FMT-10B as appropriate`
