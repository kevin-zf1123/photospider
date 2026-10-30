---
spec_schema_version: 1
id: NBL-02E
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: NBL-02
function: chroma_oklab_preserve_gray
execution: tile
proposed_operation_keys:
  - blend.chroma_oklab_preserve_gray
---

# NBL-02E: Chroma blend with preserve_gray in oklab

Inherit the [shared contract](COM_common_contract.md) and [NBL contract](NBL_nonseparable_contract.md). The proposed basic tile operator is `blend.chroma_oklab_preserve_gray`.

## Interface

`background: T[S,3]`, `source: T[S,3]`, `neutral_threshold: T[1]` -> `color: T[S,3]`.

Matching native OKLab. neutral_threshold>=0 in opponent units; suggested 0.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Use the native chroma equation in the NBL contract. When background chroma <= neutral_threshold, copy the complete background tuple.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Read the model components required by the branch; output remains in the same native color model.

## Acceptance

For B=(0.5,0,0), S=(0.25,0,1), threshold=0, output (0.5,0,0). The zero_hue counterpart yields (0.5,1,0).

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
