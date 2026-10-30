---
spec_schema_version: 1
id: FMT-11A
parent_id: FMT-11
function: xyz_to_cielab
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.xyz_to_cielab_strict
  - color.xyz_to_cielab_accelerated_apple_silicon
  - color.xyz_to_cielab_accelerated_x86_64
---

# FMT-11A: XYZ to normalized CIELAB

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: Relative XYZ with explicit white Y=1. Target: normalized l,a*,b*. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: white from source; raw requires explicit valid white. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use f from the math contract; l=(116*f(Y)-16)/100. Preserve source white and reference without adaptation.

## Behavior

Sample support: l uses Y; a* uses X,Y; b* uses Y,Z. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Acceptance

Under white xy=(0.25,0.25), XYZ=(1,1,2) produces (1,0,0). Test both floating neighbors of the l=0.08 inverse threshold, negative XYZ and l-only tiles whose formulas do not consume X/Z. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`FMT-09 decode -> FMT-10A -> optional explicit FMT-10C -> A`
