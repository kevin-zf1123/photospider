---
spec_schema_version: 1
id: FMT-11B
parent_id: FMT-11
function: cielab_to_xyz
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.cielab_to_xyz_strict
  - color.cielab_to_xyz_accelerated_apple_silicon
  - color.cielab_to_xyz_accelerated_x86_64
---

# FMT-11B: Normalized CIELAB to XYZ

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: normalized l,a*,b* with explicit white. Target: relative XYZ with white Y=1. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: white from source; raw requires explicit valid white. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use fy=(100*l+16)/116 and exact g, with no separately rounded temporary L*. Output remains relative and retains the source white/reference.

## Behavior

Sample support: X uses l,a*; Y uses l; Z uses l,b*. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Acceptance

Under white xy=(0.25,0.25), (1,0,0) produces XYZ=(1,1,2). A Y-only tile with finite l does not consume NaN a*/b*; retained X additionally consumes a*. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`B -> optional explicit FMT-10C -> FMT-10B -> explicit FMT-09 encode`
