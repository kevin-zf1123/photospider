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
  - color.oklab_to_xyz_accelerated_apple_silicon
  - color.oklab_to_xyz_accelerated_x86_64
---

# FMT-11F: OKLab to relative D65 XYZ

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). The specification remains Proposed; this member is registered as a CPU Result operation.

## Interface and output inference

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor.
Source: OKLab L,a,b. Target: relative D65 XYZ, white Y=1.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
An explicit input_description selects the described path; otherwise use the
family raw selector. The output carries no description.
Member-specific statics: fixed exact inverses; no alternate matrix parameter.
All explicit-description, source-consistency, algorithm and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Use inverse(M1)*cube(inverse(M2)*OKLab), exact inverses of the selected forward matrices. Retain signed/HDR coordinates and reference.

Exact same-position support: each transformed component uses L,a,b.
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

Use exact rational inverse matrices and cubic polynomial evaluation for an independent oracle. Negative L and opposite opponent signs remain valid if requested results are finite.

Also apply common mixed-region/component, dtype/profile, signed-zero, explicit-configuration,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `F -> optional explicit FMT-10C -> FMT-10B`. The installed public helper appends the native member key to a `WorkflowDocument`; Compiler and execution then evaluate the Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
