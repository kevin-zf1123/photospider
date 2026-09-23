---
spec_schema_version: 1
id: FMT-04B
parent_id: FMT-04
function: unassociate_alpha
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - alpha.unassociate_strict
  - alpha.unassociate_accelerated_apple_silicon
  - alpha.unassociate_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-04B: import premultiplied numeric samples as straight

Inherit the complete [FMT-04 boundary contract](FMT-04_alpha_association_contract.md).
These proposed primitive keys are not aliases of the retired typed-image keys.

## Interface and representation

Semantic `input` is an explicitly described premultiplied RGB/Gray boundary
payload with its alpha in the same tensor. `values` retains shape, dtype, channel
positions and alpha bits, and describes the converted group as straight.
Already-straight selected input fails preflight. Override can supply an explicit
call-local premultiplied source interpretation; it does not silently infer that
interpretation from sample values or alter another consumer's input.

When other complete groups remain premultiplied, the result remains a boundary
payload until they are explicitly converted. Once all complete groups are
straight, the tensor can be a canonical image. This is a metadata check without
reading unrequested other groups. Float32/Float64 and strict/named CPU profiles
inherit the family. No semantic external alpha source or retained binding exists.

## Mathematical specialization

For requested p and internal a, read both; require finite p and finite a in
[0,1]. At a=+0/-0, require p=+0/-0 and output +0. Nonzero p with zero alpha fails
InvalidDomain. For every a>0, output RN_t(p/a) in strict, or the inherited NUM
profile result. No epsilon or clipping is permitted. Nonfinite semantic output
fails ArithmeticOverflow; gradual underflow and signed zeros follow NUM.

Copy alpha and other components exactly. Lost hidden colors cannot be recovered
from zero premultiplied samples. Encoded payloads are unassociated as stored;
transfer decoding, when needed, is a subsequent explicit stage.

Raw applies NUM-05D divide(p,a) with the stated operand order and optional explicit
plane/scalar weights. It skips semantic zero clearing and finite/range checks;
0/0, division by zero and overflow follow NUM. It does not certify a straight
image or change association interpretation merely because division occurred.

## Dependencies, resources and failures

Each requested converted component reads itself and mapped alpha, including at
alpha=0/1. No other color peers join validation. Pass-through alpha/AOV requests
copy only their requested source samples; static representation/shape checks
still apply. Alpha dirty support fans out to observed selected components;
external/scalar raw weights have the family maps. There is no source snapshot
lookup through result metadata.

If B consumes A in a DAG, B's requested color makes an explicit request for both
A's corresponding color and A's alpha channel. If the input is a detached partial
payload without alpha coverage, fail by normal missing-coverage rules. Never
reuse the alpha that A originally read through hidden metadata. Apply the family
materialization, resources, cancellation, cache and error contracts. Saturation
and automatic straight pass-through are not fallback modes.

## Independent fixtures and acceptance

Premultiplied Float32 samples [-1,0.25,2,0.5] yield straight [-2,0.5,4,0.5].
For p=-0 and a=-0, output color is +0 and copied alpha remains -0. For p=0.25
and a=0, semantic evaluation fails InvalidDomain. Requested nonfinite p/alpha
or alpha outside [0,1] fails even if raw division would yield a finite value.

Float32 p=a=2^-149 gives 1; p=1, a=2^-149 fails semantic ArithmeticOverflow,
whereas raw succeeds with +Inf. Repeat Float64 at 2^-1074. At a=1, preserve
finite p and signed-zero bits after validation. Tiny positive alpha is never
routed through the zero-alpha branch.

For alpha=0 with p_R=0 and unrequested p_G=0.25, R-only succeeds with +0;
a subsequent G request fails locally without revoking completed R. Required
upstream Whole work retains its original scope. Check alpha-only and AOV-only
copies, noncanonical channel positions, exact A->B demands and detached missing
alpha failure. Check produced coverage and final owner release after context
retirement, without expecting unproduced samples to reappear.

Raw +1/-0 -> -Inf, 0/0 -> NUM NaN, both-operand NaN priority and successful
floating overflow use the independent NUM bit oracle. Semantic zero predicates
and output representation remain exact across profiles.

Conceptual DAG: explicit premultiplied payload with internal alpha -> B ->
straight image consumer. Implementation must deliver public compile/execute
examples plus independent numeric, region, metadata, budget and lifetime checks.
No implementation or benchmark result is claimed here.
