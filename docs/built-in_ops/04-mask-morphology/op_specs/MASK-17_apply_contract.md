---
spec_schema_version: 1
id: MASK-17
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-17: Result masking, alpha application and restricted sampling

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

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
For every selected c,p, read and validate m and both numeric operands, including
m=0 and m=1. Both operands must be finite. Return
RN_T((1-m)*original+m*processed) as the complete expression at every weight.
There are no endpoint copy or zero-weight input-omission branches. NaN/Inf in
either selected operand fails even when its coefficient is zero. Arithmetic zero
uses the NUM arithmetic-zero rule; no raw signed-zero or payload copy is promised.
For semantic image mixing,
author explicit FMT conversion and choose channels/alpha policy; averaging straight
RGBA coordinates is not a premultiplied Porter-Duff blend. Applicable metadata
may be retained without inherited sample-validity guarantees, as FMT specifies.

B inputs image,mask with the same shapes/dtypes as A and channels selector.
Unselected channels copy image without mask reads. Every selected output reads
and validates both mask and image, including m=0 and m=1, and returns RN_T(m*image).
Selected image samples must be finite at every weight. A zero coefficient does
not suppress invalid input or its required upstream observation. This is numeric
multiplication under the NUM rounding rules, not general alpha compositing.

C inputs image:Field[C,H,W], mask:Coverage[H,W], same float dtype; output
values:Field[C,H,W] and valid:Binary[H,W]. Required footprint statics from MASK-05;
empty_policy zero|error. For each p use S=(p+B) intersect D, with no virtual
outside samples. M=sum_{q in S}mask(q). If M>0 return
RN_T(sum_{q in S} mask(q)*image(c,q)/M), valid=1.
If M=0, zero policy gives +0 in every requested value and valid=0; error policy
fails requested values but the independently requested valid flag still reports0.
This difference is per-output semantics, subject to host publication granularity.
For requested values, every image sample in S for the requested channel is
Data/finite Validation, including zero-weight samples and the M=0 case. Every
mask sample in S is Control/Validation. Validate the full support and construct
the weighted numerator before applying empty_policy. Requesting valid alone
requires the mask support but no image payload, since it requests no mean value.
The denominator is the exact sum of all weights. Zero-weight finite samples
contribute zero, but NaN/Inf at those samples fails a requested mean value.
A requires same-position original, processed and mask; C has the declared halo.
Reference O(Q*C*|B|), exact rational sums; O(|B|) mask/accumulator state plus
budgeted limbs. A/B/D are O(Q) selected scalar work, constant scratch.

D input image Float32/64[C,H,W] with straight color description and an INTERNAL
alpha channel, plus same-dtype Coverage[H,W]. Required alpha_channel Int64 index
must match effective description (respect|override following FMT). No separate
external alpha attachment is created. Color/AOV outputs copy exactly and read
only their original source component. Requested alpha reads/validates original
alpha and mask as finite [0,1], returns RN_T(alpha*mask); mask=1 selects original
alpha bits. Do not multiply straight color channels, do not unassociate/reassociate,
do not infer alpha from C=4, and do not import old external-association metadata.
The output retains the same straight group/alpha role and establishes validity
only for actually checked alpha observations. A color-only request does not
trigger hidden alpha/mask validation.

For local members Data/Control/Validation are the sets above; descriptor
inference checks even unused ports. Dirty mapping is their transpose. A/B
selected-channel support is independent of mask values. C requested-value
support is the full in-canvas footprint. Templates must preserve the specified
NUM rounding boundary; separately rounded multiply/add passes are not implied
by evaluation of the complete mathematical expression.

## Sources, independent evidence and review boundary

[S01](../research-sources.md#s01); [S21](../research-sources.md#s21)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
