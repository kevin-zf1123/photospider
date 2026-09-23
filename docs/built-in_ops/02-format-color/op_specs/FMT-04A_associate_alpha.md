---
spec_schema_version: 1
id: FMT-04A
parent_id: FMT-04
function: associate_alpha
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - alpha.associate_strict
  - alpha.associate_accelerated_apple_silicon
  - alpha.associate_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-04A: export one group as premultiplied numeric samples

Inherit the complete [FMT-04 boundary contract](FMT-04_alpha_association_contract.md).
These proposed primitive keys are not aliases of the retired typed-image keys.

## Interface and representation

Semantic `input` contains one selected straight RGB/Gray group and its distinct
internal alpha plane. Output `values` has identical shape, dtype and channel
positions, with the selected group described as premultiplied boundary samples.
It is not a canonical complete image. Use FMT-02/05 first when alpha starts as a
separate plane/scalar or the original image has no alpha. No semantic external
alpha port or persistent alpha relation is accepted. Unselected groups/AOVs pass
through, retaining their applicable interpretation.

Float32/Float64 and strict/named CPU profiles inherit the family. Override is a
call-local source reinterpretation; it does not turn the adapter output into
straight or bypass the internal-alpha requirement. Repeated association of an
already premultiplied selected group fails preflight.

## Mathematical specialization

Read each requested color x and internal a, requiring finite x and finite a in
[0,1]. At either signed zero alpha, output color +0; otherwise strict output is
RN_t(x*a). Copy alpha and nonparticipating samples bit-for-bit. Do not skip x
validation at alpha=0. Signed/HDR colors are legal and no [0,a] clamp applies.
Encoded C=0.5 with a=0.5 produces P=0.25 without transfer decoding. Hidden color
at zero alpha is lost in this boundary conversion; no auxiliary capture is made.
A binary Gray stored-value guarantee cannot generally survive multiplication.

Raw instead uses NUM-05C multiply(x,a), permits explicit internal/plane/scalar
weights and follows NUM NaN/Inf/signed-zero/overflow behavior. It does not apply
semantic zero clearing or declare successful premultiplied boundary conversion.
Preserve applicable descriptions without sample-validity guarantees, following
the family and FMT-common raw policy.

## Dependencies, resources and failures

Requested converted samples read exactly their own source component plus alpha;
pass-through-only samples read only themselves. Both operands are required even
at alpha=0/1. Dirty alpha fans out to observed selected-group components; raw
scalar alpha affects all observed selected samples. All group/state/shape checks
remain static. The family's materialized storage, bounded algorithm, NUM error
attribution, cancellation and cache policy apply without additional exceptions.

Result metadata references the output alpha slot only. R-only production does
not publish alpha, and it does not retain an external alpha reference. Downstream
work must explicitly demand the result alpha channel. Missing detached coverage
remains missing. Normal owned samples/read windows survive context retirement.

## Independent fixtures and acceptance

Straight Float32 RGBA [-2,0.5,4,0.5] yields boundary samples [-1,0.25,2,0.5].
For the same finite colors and alpha=-0, color outputs are +0 and alpha remains
-0. Requested NaN color at alpha=0 fails; unrequested NaN peers do not add a
failure to a valid requested component.

For Float32 x=1 and a=2^-149, output is 2^-149. For x=0.5 with that alpha,
ties-to-even yields +0; x=-0.5 yields -0. Repeat Float64 at 2^-1074. At a=1,
finite samples including -0 preserve bits after validation. These products can
underflow and later unassociation need not recover their original colors.

For a Gray+alpha tensor with samples [8,0.25] and [2,1], output samples are
[2,0.25] and [2,1]. Keep the internal alpha channel in both shape and metadata;
external alpha must first be explicitly assembled. Check a deliberately
noncanonical axis/channel map and R-only, alpha-only and AOV-only requests.
Alpha-only copy may preserve a NaN bit pattern without color-domain validation.

Raw fixtures include Inf*0 -> NUM NaN, -0*1 -> -0, operand-order NaN priority
and weights outside [0,1]. Semantic and raw results cannot share an incorrect
cache identity. A normal canonical-image consumer rejects A's boundary result;
B explicitly restores straight interpretation, subject to its required coverage.

Conceptual DAG: straight image with internal alpha -> A -> export adapter.
Implementation must supply runnable public commands and independent bit/domain,
region, low-budget, cancellation and lifetime checks; no runtime result is claimed.
