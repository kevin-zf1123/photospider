---
spec_schema_version: 1
id: FMT-11H
parent_id: FMT-11
function: oklch_to_oklab
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.oklch_to_oklab_strict
  - color.oklch_to_oklab_accelerated_apple_silicon
  - color.oklch_to_oklab_accelerated_x86_64
---

# FMT-11H: OKLCh to OKLab

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: OKLCh L,C,h. Target: OKLab L,a,b. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: source input_hue_unit; explicit in raw. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Use the inverse polar formula, retaining the real hue winding input. Semantic C=0 requires finite hue as a caller precondition and produces +0; C<0 is outside the semantic precondition.

## Behavior

Sample support: L copies alone; a/b each use C,h. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Acceptance

C=2,h=2.5 pi_multiple gives a=+0,b=2. Compare multi-turn and equivalent-angle inputs without claiming that forward conversion restores winding. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`H -> F -> FMT-10B`
