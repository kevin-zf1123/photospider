---
spec_schema_version: 1
id: KEY-02B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-02
function: lab_chroma_key
execution: tile
proposed_operation_keys:
  - keying.lab_chroma_key
---

# KEY-02B: CIELAB chroma-only distance key

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.lab_chroma_key`.

## Interface

`color: T[H,W,3]`, `key_color: T[3]`, `inner: T[1]`, `outer: T[1]` -> `matte: T[H,W]`.

Native CIELAB with the same explicit white; l=L*/100. 0<=inner<outer in a*/b* distance units.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

Compute d=sqrt((a-a_key)^2+(b-b_key)^2) and publish d to T. Return h(inner,outer,d), using the stored d and KEY-01 smoothstep. Near-key colors yield 0; distant colors yield 1.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Lightness samples are outside the mathematical support.

## Acceptance

The key color gives 0. At distance>=outer output 1. A change confined to l leaves this chroma-only key unchanged.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
