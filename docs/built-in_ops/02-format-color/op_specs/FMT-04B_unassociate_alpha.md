---
spec_schema_version: 1
id: FMT-04B
parent_id: FMT-04
function: unassociate_alpha
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - alpha.unassociate_strict
  - alpha.unassociate_accelerated_apple_silicon
  - alpha.unassociate_accelerated_x86_64
---

# FMT-04B: import premultiplied numeric samples as straight

The registry provides `alpha.unassociate_<profile>` for the three CPU profiles. The operation preserves the declared tensor shape, dtype and channel positions.

Inherits the [FMT-04 boundary contract](FMT-04_alpha_association_contract.md). The operation accepts one single-tensor Result, with an optional second input only for raw explicit plane/scalar weights.

## Interface

Semantic `input` is an explicitly described premultiplied RGB/Gray boundary payload with its alpha in the same tensor. `values` retains shape, dtype, channel positions and alpha bits, and computes straight samples for the selected group. No description is attached. Already-straight selected input is rejected during preparation. `input_description` supplies the explicit call-local premultiplied source interpretation; it is not inferred from sample values.

When other complete groups remain premultiplied, the result remains a boundary payload until they are explicitly converted; once all complete groups are straight, the tensor can be a canonical image. Float32/Float64 and strict/named CPU profiles inherit the family. No semantic external alpha source exists.

## Mathematics

For each evaluated p and internal a, both are read. The caller supplies finite p and finite a in [0,1]. At a=+0/-0, p must also be signed zero; output is +0. For every a>0, output RN_t(p/a) in strict, or the inherited NUM profile result. No epsilon or clipping is permitted. The caller guarantees a finite rounded semantic result; gradual underflow and signed zeros follow NUM.

Alpha and other components are copied exactly. Lost hidden colors cannot be recovered from zero premultiplied samples. Encoded payloads are unassociated as stored; transfer decoding is a subsequent explicit stage.

Raw mode applies NUM-05D divide(p,a) with the stated operand order and optional explicit plane/scalar weights. It omits semantic zero clearing and the semantic finite-color or normalized-coverage premises; raw 0/0, division by zero and overflow follow NUM. Division does not certify a straight image.

## Behavior

Each output sample depends on the participating color and its mapped alpha, including at alpha=0/1; pass-through slots depend only on their own source. When B consumes A, its dependency requires A's corresponding color and alpha output. No previous input alpha is recovered.

## Acceptance

Premultiplied Float32 samples [-1,0.25,2,0.5] yield straight [-2,0.5,4,0.5]. For p=-0 and a=-0, output color is +0 and copied alpha remains -0. For p=0.25 and a=0, the semantic caller precondition is violated; nonfinite p/alpha or alpha outside [0,1] likewise lie outside the semantic domain.

Float32 p=a=2^-149 gives 1; p=1, a=2^-149 violates the finite semantic-result premise, whereas raw succeeds with +Inf. Repeat Float64 at 2^-1074. At a=1, finite p and signed-zero bits are preserved. Tiny positive alpha never takes the zero-alpha branch. For alpha=0 and p=0, semantic output is +0.

Raw +1/-0 -> -Inf, 0/0 -> NUM NaN, both-operand NaN priority and successful floating overflow follow the NUM bit definitions. Check alpha/AOV-only copy, noncanonical placement and explicit A-to-B composition.

## Conceptual DAG

explicit premultiplied payload with internal alpha -> B -> straight image consumer
