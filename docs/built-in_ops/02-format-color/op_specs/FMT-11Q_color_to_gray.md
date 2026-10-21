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

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). The specification remains Proposed; this member is registered as a CPU Result operation.

## Interface and output inference

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor.
Source: XYZ, NCL YCbCr, normalized CIELAB or OKLab. Target: linear_y, encoded_luma, cielab_l or oklab_l Gray.
Q/R structural remapping and axis rules apply exactly as specified in the family.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
An explicit input_description selects the described path; otherwise use the
family raw selector. The output carries no description.
Member-specific statics: gray_kind inferred semantically, explicit in raw.
All explicit-description, source-consistency, algorithm and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Copy the native coordinate; the caller supplies the corresponding Gray configuration downstream. Contract three selected slots into one at their smallest physical index; retain channel axis and other channels.

Exact same-position support: only selected Y/Y′/l/L is consumed mathematically; physical source tiles may cover discarded chroma.
Take the union over every sample of each complete retained output tile, then close on the source grid. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests.
No halo, hidden-color skip, alpha validation or full-image scan is introduced.
Strict evaluates the complete specified formula with its stated copy/selection
exceptions; accelerated numerical rules, raw specials and errors inherit the
family/math contract. The normalized Lab lightness is l=L*/100 wherever relevant.
Semantic sample validity, finite rounded results and nonsingular semantic formulas are caller preconditions over complete retained output tiles. Fixed execution does not validate these numerical premises; operational errors retain the kernel category.

## Storage, resources and implementation identity

This is a registered native Result primitive. Internal helper reuse preserves its declared support, rounding and static checks.
Auto/materialize copy into independent dense output tiles. A nonempty forced view task reads only schema, uses zero scratch and fails InvalidArgument/ViewUnavailable.
Generic and spatial layouts use complete output tiles and source-grid closure. Copies use zero scratch; Empty has no payload tasks. Successful calls publish in order.
The implementation uses fixed invocation ABI 8 and the registered CPU profiles.

## Independent acceptance and conceptual workflow

Lab=(0.5,NaN,10) permits Gray l=0.5. Test permuted/interleaved logical group indices and alpha/AOV remapping independently of the physical layout.

Also apply common mixed-region/component, dtype/profile, signed-zero, explicit-configuration,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
Conceptual paths are `linear RGB -> FMT-10A -> Q` for luminance Gray,
`relative XYZ -> A or E -> Q` for the corresponding lightness Gray (E requires
D65), and `encoded RGB -> M -> Q` for luma Gray. The installed public helper appends the native member key to a `WorkflowDocument`; Compiler and execution then evaluate the Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
