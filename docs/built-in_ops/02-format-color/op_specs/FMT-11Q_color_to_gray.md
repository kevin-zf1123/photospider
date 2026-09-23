---
spec_schema_version: 1
id: FMT-11Q
parent_id: FMT-11
function: color_to_gray
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.color_to_gray_strict
  - color.color_to_gray_accelerated_apple_silicon
  - color.color_to_gray_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-11Q: Described color to corresponding Gray

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). These Proposed
keys/helpers are unimplemented and create no runtime compatibility alias.

## Interface and output inference

One `input` tensor Value produces `values`, preserving Float32/Float64 dtype.
Source: XYZ, NCL YCbCr, normalized CIELAB or OKLab. Target: linear_y, encoded_luma, cielab_l or oklab_l Gray.
Q/R structural remapping and axis rules apply exactly as specified in the family.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
Semantic mode selects a complete group; raw uses the family ordered selector.
Member-specific statics: gray_kind inferred semantically, explicit in raw.
All generic mode, source consistency, metadata and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Copy the native coordinate and publish complete Gray meaning. Contract three selected slots into one at their smallest physical index; retain channel axis and other channels.

Exact same-position support: only selected Y/Y′/l/L; discarded chroma is not read.
Take unions for multiple requested outputs and the forward relation for dirty
mapping. Static metadata checks apply even to Empty/bypass-only requests.
No halo, hidden-color skip, alpha validation or full-image scan is introduced.
Strict evaluates the complete specified formula with its stated copy/selection
exceptions; accelerated numerical rules, raw specials and errors inherit the
family/math contract. The normalized Lab lightness is l=L*/100 wherever relevant.
Invalid consumed semantics or an explicit singularity fail the requested
publication; unrelated unobserved sample errors do not. Preserve upstream scope.

## Storage, resources and implementation identity

This is a native primitive; internal helper reuse must preserve its declared support, rounding and validation.
auto can share a legal same-owner view while retaining validation; forced view fails if that mapping cannot be represented.
Inherit canonical planar pages, full-image virtual reservation, exact coverage,
immutable ownership/lifetime, budgeted scratch/refinement and cancellation.
Costs and CPU profile support are the family contract, not a Whole fallback.
No implementation or runtime test evidence is claimed here.

## Independent acceptance and conceptual workflow

Lab=(0.5,NaN,10) permits Gray l=0.5. Test permuted/interleaved logical group indices and alpha/AOV remapping; physical backing remains planar.

Also apply common mixed-region/component, dtype/profile, signed-zero, metadata,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
Conceptual paths are `linear RGB -> FMT-10A -> Q` for luminance Gray,
`relative XYZ -> A or E -> Q` for the corresponding lightness Gray (E requires
D65), and `encoded RGB -> M -> Q` for luma Gray. When implemented, bind a small planar
input through the public Compiler/ExecutionContext, request selected color and
bypass components separately, and check values, metadata, support and failures.
This is a future workflow obligation, not runnable code against nonexistent APIs.
Primary sources and their compatibility limits are linked in the math supplement.
