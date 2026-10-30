---
spec_schema_version: 1
id: KEY-06A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
parent_id: KEY-06
function: foreground_regularized
execution: tile
proposed_operation_keys:
  - keying.foreground_regularized
---

# KEY-06A: Regularized foreground color estimate

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.foreground_regularized`.

## Interface

`observed: T[H,W,3]`, `alpha: T[H,W]`, `background: T[H,W,3]`, `prior: T[H,W,3]`, `regularization: T[1]` -> `foreground: T[H,W,3]`.

Matching linear RGB. Alpha in [0,1]; lambda=regularization>0. The prior and background are explicit evidence inputs.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

## Mathematics

At alpha=0 copy prior; at alpha=1 copy observed. Otherwise F=(alpha*(I-(1-alpha)*B)+lambda*prior)/(alpha^2+lambda) per channel. Endpoints are explicit overrides of the interior regularized objective.

## Behavior

E support at matching coordinates. Source descriptors are static dependencies; payload dependencies follow the component relations in the formula. Empty requests read no payload. Alpha remains a separate unchanged input available for explicit downstream image assembly. An endpoint reads only the selected color plus its alpha control.

## Acceptance

alpha=0/1 copies the corresponding input. For alpha=0.5, I=0.5, B=0, prior=1, lambda=0.25, output 1.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
