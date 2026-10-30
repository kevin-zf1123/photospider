---
spec_schema_version: 1
id: FMT-11D
parent_id: FMT-11
function: cielch_to_cielab
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.cielch_to_cielab_strict
  - color.cielch_to_cielab_accelerated_apple_silicon
  - color.cielch_to_cielab_accelerated_x86_64
---

# FMT-11D: CIELCh(ab) to normalized CIELAB

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). This member is a CPU Result operation with strict, accelerated Apple Silicon and accelerated x86-64 profiles.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: normalized l,C*,h. Target: normalized l,a*,b*. Shape, channel axis/count and slots are preserved; target roles follow the selected source role order. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. Member-specific statics: source input_hue_unit; explicit in raw. Explicit-description, source-consistency, algorithm and layout fields follow the family.

## Mathematics

Accept finite nonnegative semantic C* and arbitrary finite winding hue. C*=0 still requires finite hue as a caller precondition and produces +0 opponents; raw applies numeric products.

## Behavior

Sample support: l copies alone; a*/b* each use C*,h. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests. There is no halo, hidden-color skip, alpha validation or full-image scan. Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions. This member materializes transformed output and rejects forced view, even for a bypass-only request. Raw specials and errors follow the family and math contract.

## Acceptance

C*=2,h=2.5 pi_multiple gives a*=+0,b*=2. A NaN hue is outside the semantic a* precondition even at C*=0; l-only remains independent. Also cover mixed component requests, both dtypes and signed zeros; an oracle uses the actual stored floating inputs, not ideal decimal inputs.

## Conceptual DAG

`D -> B -> FMT-10B`
