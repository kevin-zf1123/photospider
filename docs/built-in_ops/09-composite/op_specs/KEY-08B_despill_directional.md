---
spec_schema_version: 1
id: KEY-08B
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-08
function: despill_directional
execution: tile
proposed_operation_keys:
  - keying.despill_directional
---

# KEY-08B: Suppress a specified chromatic spill direction

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.despill_directional`.

## Interface

`foreground: T[S,C]`, `spill: T[3]`, `mask: T[S,1]`, `strength: T[1]` -> `foreground: T[S,C]`.

Select linear RGB with explicit working-basis Y row r resolved to Float64, and normalize w=r/sum(r) with sum(r)>0. mask,strength in [0,1]; suggested strength=0. Spill has a nonzero chromatic direction. The oracle uses the exact normalization of these stored coefficients.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

d=spill-dot(w,spill)*(1,1,1); e=max(0,dot(d,F-dot(w,F)*(1,1,1))/dot(d,d)); F_out=F-strength*mask*e*d. Since sum(w)=1, the ideal equation retains dot(w,F). Copy alpha and AOVs; zero correction copies.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. RGB output uses its complete source tuple and spill direction. Alpha/AOV output is independent.

## Acceptance

Neutral RGB is unchanged. Verify linear Y retention within floating tolerance and alpha bit preservation. Negative output RGB is allowed.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
