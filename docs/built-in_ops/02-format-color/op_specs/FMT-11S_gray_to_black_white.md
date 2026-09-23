---
spec_schema_version: 1
id: FMT-11S
parent_id: FMT-11
function: gray_to_black_white
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_template_names:
  - color.gray_to_black_white
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-11S: Gray to binary Black/White

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). These Proposed
keys/helpers are unimplemented and create no runtime compatibility alias.

## Interface and output inference

One `input` tensor Value produces `values`, preserving Float32/Float64 dtype.
Source: one explicitly described native Gray component. Target: binary Black/White retaining Gray origin.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
Semantic mode selects a complete group; raw uses the family ordered selector.
Member-specific statics: required finite Float64 threshold; no implicit .5.
All generic mode, source consistency, metadata and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Compile hard x>=threshold, semantic finite validation and exact typed 0/1 selection plus metadata. Retain shape/slots. The current Whole mask.threshold is not a conforming implementation.

Exact same-position support: selected Gray only; all other channels bypass.
Take unions for multiple requested outputs and the forward relation for dirty
mapping. Static metadata checks apply even to Empty/bypass-only requests.
No halo, hidden-color skip, alpha validation or full-image scan is introduced.
Strict evaluates the complete specified formula with its stated copy/selection
exceptions; accelerated numerical rules, raw specials and errors inherit the
family/math contract. The normalized Lab lightness is l=L*/100 wherever relevant.
Invalid consumed semantics or an explicit singularity fail the requested
publication; unrelated unobserved sample errors do not. Preserve upstream scope.

## Storage, resources and implementation identity

This is a compile-time composition with no native registry key; the family specifies constituent admission and exact observable behavior.
This member materializes transformed output; whole-node forced view is rejected even for a bypass-only request.
Inherit canonical planar pages, full-image virtual reservation, exact coverage,
immutable ownership/lifetime, budgeted scratch/refinement and cancellation.
Costs and CPU profile support are the family contract, not a Whole fallback.
No implementation or runtime test evidence is claimed here.

## Independent acceptance and conceptual workflow

Gray l=[0.25,0.5,0.75], threshold=0.5 yields [0,1,1]. Semantic NaN fails; raw NaN compares false. Integer packing stays in codecs.

Also apply common mixed-region/component, dtype/profile, signed-zero, metadata,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `Q -> S -> explicit FMT-06/codec if needed`. When implemented, bind a small planar
input through the public Compiler/ExecutionContext, request selected color and
bypass components separately, and check values, metadata, support and failures.
This is a future workflow obligation, not runnable code against nonexistent APIs.
Primary sources and their compatibility limits are linked in the math supplement.
