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

The default registry provides `alpha.unassociate_<profile>` for the three CPU
profiles. The operation uses fixed invocation ABI 8 and preserves the declared tensor
shape, dtype and channel positions. The specification remains Proposed. Validate the public operation with temporary independent numerical and ownership checks when changing it.

Inherit the complete [FMT-04 boundary contract](FMT-04_alpha_association_contract.md).
The operation accepts one single-tensor Result, with optional second input only for raw explicit plane/scalar weights.

## Interface and representation

Semantic `input` is an explicitly described premultiplied RGB/Gray boundary
payload with its alpha in the same tensor. `values` retains shape, dtype, channel
positions and alpha bits, and computes straight samples for the selected group. No description is attached.
Already-straight selected input fails preflight. `input_description` supplies the explicit
call-local premultiplied source interpretation; it does not silently infer that
interpretation from sample values or alter another consumer's input.

When other complete groups remain premultiplied, the result remains a boundary
payload until they are explicitly converted. Once all complete groups are
straight, the tensor can be a canonical image. This downstream interpretation is explicit and does not
require reading unrequested other groups. Float32/Float64 and strict/named CPU profiles
inherit the family. No semantic external alpha source or retained binding exists.

## Mathematical specialization

For each evaluated p and internal a, read both. The caller supplies finite p
and finite a in [0,1]. At a=+0/-0, p must also be signed zero; output is +0. For every a>0, output RN_t(p/a) in strict, or the inherited NUM
profile result. No epsilon or clipping is permitted. The caller guarantees a finite rounded semantic result; gradual underflow
and signed zeros follow NUM.

Copy alpha and other components exactly. Lost hidden colors cannot be recovered
from zero premultiplied samples. Encoded payloads are unassociated as stored;
transfer decoding, when needed, is a subsequent explicit stage.

Raw applies NUM-05D divide(p,a) with the stated operand order and optional explicit
plane/scalar weights. It skips semantic zero clearing and finite/range checks;
0/0, division by zero and overflow follow NUM. It does not certify a straight
image or change association interpretation merely because division occurred.

## Dependencies, resources and failures

Each complete retained output tile declares original participating colors and
mapped alpha, including at alpha=0/1. Pass-through slots declare their own source
only. The input-grid closures may grant larger source tiles without evaluating
extra peers. Static checks remain mandatory; numerical validity belongs to the
caller. The family's fixed allocations, scratch and ordered retirement apply.

When B consumes A, its color dependency explicitly requires A's corresponding
color and alpha output tiles. Missing detached coverage fails through normal
binding rules. No previous input alpha is recovered through metadata. Output
Results retain their independent allocations after context retirement.

## Independent fixtures and acceptance

Premultiplied Float32 samples [-1,0.25,2,0.5] yield straight [-2,0.5,4,0.5].
For p=-0 and a=-0, output color is +0 and copied alpha remains -0. For p=0.25
and a=0, the semantic caller precondition is violated. Nonfinite participating
p/alpha or alpha outside [0,1] likewise lies outside the semantic domain.

Float32 p=a=2^-149 gives 1; p=1, a=2^-149 violates the finite semantic-result premise,
whereas raw succeeds with +Inf. Repeat Float64 at 2^-1074. At a=1, preserve
finite p and signed-zero bits. Tiny positive alpha is never
routed through the zero-alpha branch.

For alpha=0 and evaluated p=0, semantic output is +0. Every participating
coordinate in a retained output tile must satisfy the premise, including peers
added by tile closure. Check alpha/AOV-only copy tasks, noncanonical placement,
explicit A-to-B dependencies and detached missing-alpha failures.

Raw +1/-0 -> -Inf, 0/0 -> NUM NaN, both-operand NaN priority and successful
floating overflow use the independent NUM bit oracle. Semantic zero predicates
and output representation remain exact across profiles.

Conceptual DAG: explicit premultiplied payload with internal alpha -> B ->
straight image consumer. The native Result implementation uses the public helper and registry contract described above. Runtime and performance claims are limited to checks actually run on the target host.
