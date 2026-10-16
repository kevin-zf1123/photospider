---
spec_schema_version: 1
id: FMT-11S
parent_id: FMT-11
function: gray_to_black_white
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_public_helper
clarification_status: complete
proposed_template_names:
  - color.gray_to_black_white
---

# FMT-11S: Gray to binary Black/White

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). The specification remains Proposed. This public helper appends the registered `mask.threshold_channel_<profile>` operation; there is no native color-model key for S.

## Interface and output inference

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor.
Source: one explicitly described native Gray component. Target: binary Black/White; the caller retains Gray origin in configuration.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
An explicit input_description selects the described path; otherwise use the
family raw selector. The output carries no description.
Member-specific statics: required finite Float64 threshold; no implicit .5.
All explicit-description, source-consistency, algorithm and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Compile the hard `x >= threshold` comparison, semantic finite validation and exact typed 0/1 selection without attached metadata. Retain shape and slots. The helper appends the registered MASK channel-threshold operation.

Exact same-position support: selected Gray only; all other channels bypass.
Take unions for multiple requested outputs and the forward relation for dirty
mapping. Static structure and explicit-configuration checks apply even to Empty/bypass-only requests.
No halo, hidden-color skip, alpha validation or full-image scan is introduced.
Strict evaluates the complete specified formula with its stated copy/selection
exceptions; accelerated numerical rules, raw specials and errors inherit the
family/math contract. The normalized Lab lightness is l=L*/100 wherever relevant.
Invalid consumed semantics or an explicit singularity fail the requested
publication; unrelated unobserved sample errors do not. Preserve upstream scope.

## Storage, resources and implementation identity

The helper appends the registered MASK threshold Result operation. The lowering target uses Result ABI 5. This operation materializes output and rejects forced view. Empty demand is stateless and publication is transactional.

## Independent acceptance and conceptual workflow

Gray l=[0.25,0.5,0.75], threshold=0.5 yields [0,1,1]. Semantic NaN fails; raw NaN compares false. Integer packing stays in codecs.

Also apply common mixed-region/component, dtype/profile, signed-zero, explicit-configuration,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `Q -> S -> explicit FMT-06/codec if needed`. The installed public helper appends `mask.threshold_channel_<profile>` to a `WorkflowDocument`; Compiler and execution then evaluate that Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
