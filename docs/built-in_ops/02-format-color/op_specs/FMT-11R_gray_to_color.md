---
spec_schema_version: 1
id: FMT-11R
parent_id: FMT-11
function: gray_to_color
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_2
clarification_status: complete
registered_operation_keys:
  - color.gray_to_color_strict
  - color.gray_to_color_accelerated_apple_silicon
  - color.gray_to_color_accelerated_x86_64
---

# FMT-11R: Gray to corresponding neutral color

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). The specification remains Proposed; this member is registered as a CPU Result operation.

## Interface and output inference

One Result containing a single Float32 or Float64 tensor and no fields produces one `values` Result containing a single tensor.
Source: one of the four native Gray kinds. Target: neutral XYZ, NCL YCbCr, normalized CIELAB or OKLab.
Q/R structural remapping and axis rules apply exactly as specified in the family.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
Semantic mode selects a complete group; raw uses the family ordered selector.
Member-specific statics: Gray kind/white from metadata; explicit raw kind and linear-Y white; output_axis required for axis-free input.
All generic mode, source consistency, metadata and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Expand the Gray slot into three canonical-order slots, retaining other channels. Native linear-Y gives Y*W; other kinds give coordinate plus two +0 opponents. No original chroma is recovered.

Exact same-position support: actual Gray-dependent outputs read Gray; inserted zero opponents have no sample dependency.
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

Gray Y=0.5 with white xy=(0.25,0.25) yields XYZ=(0.5,0.5,1). Lab a*-only zero generation does not read an invalid source l.

Also apply common mixed-region/component, dtype/profile, signed-zero, metadata,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `R -> B/F/N or FMT-10B as appropriate`. The installed public helper appends the native member key to a `WorkflowDocument`; Compiler and execution then evaluate the Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
