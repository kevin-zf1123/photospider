---
spec_schema_version: 1
id: FMT-11O
parent_id: FMT-11
function: xyz_to_xyy
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.xyz_to_xyy_strict
  - color.xyz_to_xyy_accelerated_apple_silicon
  - color.xyz_to_xyy_accelerated_x86_64
---

# FMT-11O: XYZ to xyY

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). The specification remains Proposed; this member is registered as a CPU Result operation.

## Interface and output inference

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor.
Source: relative or absolute XYZ with explicit white. Target: xyY with unchanged Y units/reference.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
An explicit input_description selects the described path; otherwise use the
family raw selector. The output carries no description.
Member-specific statics: white from input_description; raw requires white_x/white_y.
All explicit-description, source-consistency, algorithm and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Use exact component sum. Semantic black adopts reference-white xy; nonzero exact cancellation is outside semantic x/y preconditions.

Exact same-position support: x/y use X,Y,Z; output Y copies only source Y.
Take the union over every sample of each complete retained output tile, then close on the source grid. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests.
No halo, hidden-color skip, alpha validation or full-image scan is introduced.
Strict evaluates the complete specified formula with its stated copy/selection
exceptions; accelerated numerical rules, raw specials and errors inherit the
family/math contract. The normalized Lab lightness is l=L*/100 wherever relevant.
Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions over complete retained output tiles. Fixed execution does not validate these numerical premises; operational errors retain the kernel category.

## Storage, resources and implementation identity

This is a registered native Result primitive. Internal helper reuse preserves its declared support, rounding and static checks.
This member materializes transformed output and rejects forced view even for a bypass-only request.
The implementation supports generic and spatial layouts. Complete output tiles determine source-grid closure and numerical preconditions. Pure copies/constants use zero scratch; arithmetic uses fixed declared scratch. Empty has no payload tasks, and successful calls publish in order.
The implementation uses fixed invocation ABI 8 and the registered CPU profiles.

## Independent acceptance and conceptual workflow

XYZ=(1,1,2) gives (0.25,0.25,1). (1,-1,0) is outside semantic x/y preconditions, while a complete retained Y-only tile remains evaluable. Raw black produces ratio NaNs.

Also apply common mixed-region/component, dtype/profile, signed-zero, explicit-configuration,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `FMT-10A -> O -> explicit xyY manipulation`. The installed public helper appends the native member key to a `WorkflowDocument`; Compiler and execution then evaluate the Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
