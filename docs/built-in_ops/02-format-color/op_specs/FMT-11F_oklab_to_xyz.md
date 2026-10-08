---
spec_schema_version: 1
id: FMT-11F
parent_id: FMT-11
function: oklab_to_xyz
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_2
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

One Result containing a single Float32 or Float64 tensor and no fields produces one `values` Result containing a single tensor.
Source: OKLab L,a,b. Target: relative D65 XYZ, white Y=1.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
Semantic mode selects a complete group; raw uses the family ordered selector.
Member-specific statics: fixed exact inverses; no alternate matrix parameter.
All generic mode, source consistency, metadata and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Use inverse(M1)*cube(inverse(M2)*OKLab), exact inverses of the selected forward matrices. Retain signed/HDR coordinates and reference.

Exact same-position support: each transformed component uses L,a,b.
Take unions for multiple requested outputs and the forward relation for dirty
mapping. Static metadata checks apply even to Empty/bypass-only requests.
No halo, hidden-color skip, alpha validation or full-image scan is introduced.
Strict evaluates the complete specified formula with its stated copy/selection
exceptions; accelerated numerical rules, raw specials and errors inherit the
family/math contract. The normalized Lab lightness is l=L*/100 wherever relevant.
Invalid consumed semantics or an explicit singularity fail the requested
publication; unrelated unobserved sample errors do not. Preserve upstream scope.

## Storage, resources and implementation identity

This is a registered native Result primitive. Internal helper reuse preserves its declared support, rounding and validation.
This member materializes transformed output and rejects forced view even for a bypass-only request.
The implementation supports generic and spatial layouts. Q permits a physical view when the complete requested mapping is representable; other members materialize and reject forced view. Empty demand is stateless, and publication is transactional.
The implementation uses Result ABI 2 and the registered CPU profiles.

## Independent acceptance and conceptual workflow

Use exact rational inverse matrices and cubic polynomial evaluation for an independent oracle. Negative L and opposite opponent signs remain valid if requested results are finite.

Also apply common mixed-region/component, dtype/profile, signed-zero, metadata,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `F -> optional explicit FMT-10C -> FMT-10B`. The installed public helper appends the native member key to a `WorkflowDocument`; Compiler and execution then evaluate the Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
