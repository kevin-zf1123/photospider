---
spec_schema_version: 1
id: FMT-11M
parent_id: FMT-11
function: rgb_to_ycbcr_ncl
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_result_abi_2
clarification_status: complete
registered_operation_keys:
  - color.rgb_to_ycbcr_ncl_strict
  - color.rgb_to_ycbcr_ncl_accelerated_apple_silicon
  - color.rgb_to_ycbcr_ncl_accelerated_x86_64
---

# FMT-11M: Encoded RGB to NCL YCbCr

Inherit the complete [family contract](FMT-11_model_conversion_contract.md),
[normative mathematics](FMT-11_model_math.md) and
[relative-coordinate scale](FMT_relative_coordinate_scale.md). The specification remains Proposed; this member is registered as a CPU Result operation.

## Interface and output inference

One Result containing a single Float32 or Float64 tensor and no fields produces one `values` Result containing a single tensor.
Source: explicitly transfer-encoded RGB. Target: full-resolution native Y′,Cb,Cr.
Preserve shape, channel axis/count and slots; target roles follow the selected source role order.
Unrelated groups, internal alpha and AOV samples pass through bit-for-bit.
Semantic mode selects a complete group; raw uses the family ordered selector.
Member-specific statics: required ncl_matrix preset or valid Kr/Kb.
All generic mode, source consistency, metadata and layout fields inherit the
family; no missing semantic interpretation is inferred from sample magnitudes.

## Mathematics, sample support and errors

Use the exact expanded NCL linear formulas. Preserve underlying RGB primaries/white/transfer separately from matrix selection. No integer bias or resampling.

Exact same-position support: each transformed output uses full R′,G′,B′.
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

Kr=Kb=0.25 and RGB=(1,0,0) gives (0.25,-1/6,0.5) before destination rounding. Equal RGB yields zero chroma.

Also apply common mixed-region/component, dtype/profile, signed-zero, metadata,
resource/cancellation and owner-lifetime acceptance where relevant. An independent
formula oracle must use actual stored floating inputs, not ideal decimal inputs.
The conceptual workflow is `FMT-09 encode -> M -> explicit FMT-06 code encoding -> codec`. The installed public helper appends the native member key to a `WorkflowDocument`; Compiler and execution then evaluate the Result operation. Request selected color and bypass components separately to observe the declared support.
Primary sources and their compatibility limits are linked in the math supplement.
