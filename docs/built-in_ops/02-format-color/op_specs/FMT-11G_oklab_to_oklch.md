---
spec_schema_version: 1
id: FMT-11G
parent_id: FMT-11
function: oklab_to_oklch
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.oklab_to_oklch_strict
  - color.oklab_to_oklch_accelerated_apple_silicon
  - color.oklab_to_oklch_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-11G: OKLab to OKLCh

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). These Proposed
keys/helpers are unimplemented and create no runtime compatibility alias.

## Interface and output inference

One `input` tensor Value produces `values`, preserving Float32/Float64 dtype.
Source: OKLab L,a,b. Target: OKLCh L,C,h.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
Semantic mode selects a complete group; raw uses the family ordered selector.
Member-specific statics: required output_hue_unit.
All generic mode, source consistency, metadata and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Use the polar formula without CIELAB scaling. Preserve fixed D65 and the selected OKLab version.

Exact same-position support: L copies alone; C/h each use a,b.
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
This member materializes transformed output; whole-node forced view is rejected even for a bypass-only request.
Inherit canonical planar pages, full-image virtual reservation, exact coverage,
immutable ownership/lifetime, budgeted scratch/refinement and cancellation.
Costs and CPU profile support are the family contract, not a Whole fallback.
No implementation or runtime test evidence is claimed here.

## Independent acceptance and conceptual workflow

a=3,b=4 yields C=5 without nominal clipping. Test signed-axis hue, tiny nonzero chroma and joint zero; L-only ignores invalid opponents.

Also apply common mixed-region/component, dtype/profile, signed-zero, metadata,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `E -> G -> explicit polar adjustment`. When implemented, bind a small planar
input through the public Compiler/ExecutionContext, request selected color and
bypass components separately, and check values, metadata, support and failures.
This is a future workflow obligation, not runnable code against nonexistent APIs.
Primary sources and their compatibility limits are linked in the math supplement.
