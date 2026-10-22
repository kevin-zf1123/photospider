---
spec_schema_version: 1
id: MASK-17
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-17: Result masking, alpha application and restricted sampling

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
All members in this Proposed family are registered in the bundled strict CPU
set.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-17A](MASK-17A_affect_result.md) | affect_result | Interpolate a processed result by a mask |
| [MASK-17B](MASK-17B_multiply_mask.md) | multiply_mask | Gate or scale selected numeric channels |
| [MASK-17C](MASK-17C_restricted_mean.md) | restricted_mean | Mask-restricted local mean with denominator validity |
| [MASK-17D](MASK-17D_apply_alpha.md) | apply_alpha | Apply coverage to internal straight-image alpha only |

## Normative shared mathematics and interface

The family provides four independently named members: affect_result,
multiply_mask, restricted_mean and apply_alpha. Each has fixed ports and
semantics; no shared mode dispatcher or arbitrary masked-filter interface is
defined. restricted_mean specifies the local weighted mean only; other
sample-restricted statistics require their own contracts.

These members intentionally separate "affect_result" from "restrict_samples".
A is a numeric result interpolation, not Porter-Duff over; C changes a local
statistic's denominator. B gates/scales arbitrary numeric samples; D changes
only a straight image's independent alpha plane. No member invokes an arbitrary
filter plugin behind an opaque mask flag. Color conversion, grading and layer
compositing remain explicit neighboring categories.

A inputs original,processed Float32/64[C,H,W] with identical dtype/shape and
compatible applicable spatial/description interpretation, plus mask Coverage[H,W]
with the same floating dtype. Required channels String selects unique indices;
unselected channels are bit copies of original, with no mask/processed reads.
For every selected c,p, consume m and both numeric operands, including
m=0 and m=1. Both operands must be finite. Return
RN_T((1-m)*original+m*processed) as the complete expression at every weight.
There are no endpoint copy or zero-weight input-omission branches. Both operands
are finite caller premises even when their coefficient is zero; no scan or
InvalidDomain diagnostic is promised. Arithmetic zero
uses the NUM arithmetic-zero rule; no raw signed-zero or payload copy is promised.
For semantic image mixing,
author explicit FMT conversion and choose channels/alpha policy; averaging straight
RGBA coordinates is not a premultiplied Porter-Duff blend. The caller supplies
any interpretation to later consumers explicitly; outputs carry no metadata or
sample-validity certificate.

B inputs image,mask with the same shapes/dtypes as A and channels selector.
Unselected channels copy image without mask reads. Every selected output reads
both mask and image, including m=0 and m=1, and returns RN_T(m*image).
Selected image samples must be finite at every weight. A zero coefficient does
not waive the finite-input premise or the declared upstream dependency. This is numeric
multiplication under the NUM rounding rules, not general alpha compositing.

C inputs image:Field[C,H,W], mask:Coverage[H,W], same float dtype; output
values:Field[C,H,W] and valid:Binary[H,W]. Required footprint statics from MASK-05;
empty_policy zero|error. For each p use S=(p+B) intersect D, with no virtual
outside samples. M=sum_{q in S}mask(q). If M>0 return
RN_T(sum_{q in S} mask(q)*image(c,q)/M), valid=1.
If M=0, zero policy gives +0 in each selected value and valid=0. The explicit
error policy fails evaluation when values are selected. Selecting only valid
computes 0 without a mean failure; selecting values and valid in the same call
shares that call's failure and publishes neither. For selected values, every
image sample in S for that channel is Data, including zero weights and M=0.
Every mask sample in S is Control. Construct the full weighted numerator before
applying empty_policy. Selecting valid alone needs mask support and no image
payload. The denominator is the exact weight sum. Finite image samples at every
weight and Coverage masks are unchecked premises, without a validation scan.

A requires same-position original, processed and mask; C has the declared halo.
Reference O(Q*C*|B|), exact rational sums; O(|B|) mask/accumulator state plus
budgeted limbs. A/B/D are O(Q) selected scalar work, constant scratch.

D input image Float32/64[C,H,W] has a caller-supplied straight-color
`input_description` with an internal alpha channel, plus same-dtype Coverage[H,W].
Required alpha_channel Int64 must match the static description. No external alpha
attachment is created. Color/AOV outputs copy their source components. Selected
alpha consumes original alpha and mask under the finite [0,1] caller premise,
returns RN_T(alpha*mask), and copies alpha bits for mask=1. Straight colors are
not multiplied or unassociated/reassociated. Alpha is not inferred from C=4.
The caller supplies the output interpretation explicitly to the next consumer;
there is no attached description or sample-validity certificate. A color-only
selection needs no alpha/mask computation or validation scan.

For local members, the sets above describe mathematical Data/Control support;
preparation checks static schemas even for unused ports. Lower complete selected
output tiles to fixed input-tile reads. A/B selected-channel support is independent
of weights, and C value support includes its entire in-canvas footprint. Templates
preserve NUM rounding boundaries; the complete expression does not imply
separately rounded multiply/add passes.

## Sources, independent evidence and review boundary

[S01](../research-sources.md#s01); [S21](../research-sources.md#s21)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
