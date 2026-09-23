---
spec_schema_version: 1
id: FMT-09B
parent_id: FMT-09
function: encode_transfer
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
proposed_operation_keys:
  - color.transfer_encode_strict
  - color.transfer_encode_accelerated_apple_silicon
  - color.transfer_encode_accelerated_x86_64
---

# FMT-09B: encode transfer

Inherit the complete [FMT-09 contract](FMT-09_transfer_contract.md) and
[scalar definitions](FMT-09_transfer_math.md). These keys are proposed native
primitives, not registered interfaces or a universal color converter.

## Interface and observable behavior

One Float32/Float64 `input` produces same-dtype, same-shape `values`. A target
curve is always explicit; gamma, BT.2020 variant and BT.1886 black/white parameters
follow the family table. Semantic source metadata must already identify the
compatible linear quantity. An encoded source is rejected rather than encoded
again, even when its existing transfer matches the chosen target.

For each requested participating q, read only input[q], perform semantic
validation or raw evaluation, and output RN_dtype(E(input[q])). Linear and
gamma=1 obey the special bit-copy identity. Unselected samples copy exactly.
No alpha multiplication, hidden-color clearing, basis/white conversion or
range normalization is implicit. Integer dtype must be converted before B in
all modes. Non-native floating code ranges require explicit decoding in semantic
mode; raw may apply E directly to their stored numeric values.

Semantic output records the selected curve, coefficients/parameters and native
linear interpretation without changing primaries/white. An ACEScct curve on
another RGB basis does not assert a complete ACEScct space. Raw retains applicable
input descriptions without marking them as correctly encoded or sample-valid.

## Dependencies, errors and storage

Inherit exact pointwise demand, invalidation, validation scope and source-state
errors. PQ accepts [0,10000] cd/m²; BT.1886 accepts [Lb,Lw]; HLG accepts [0,1].
Reference incompatibility is static; no metadata update performs a scene-to-display
rendering. Static parameter checks precede sample evaluation in all modes.

Auto/view/materialize follows the family identity rule. Semantic identity retains
its requested finite checks and independent output metadata. Nonidentity B only
materializes requested coverage, obeying planar ownership, prepared pages,
cancellation, bounded strict math and publication rules. Intrinsic ACEScc floor
samples retain their source witnesses and can still fail input validation.

## Independent acceptance fixtures

1. Gamma=2: [-4,-0,0.25,4] -> [-2,-0,0.5,2] exactly. Gamma=1 raw copies all
   bits, while semantic identity rejects nonfinite participating samples.
2. sRGB real-number anchor: E(0.0031308)=0.040449936 before final rounding.
   The decimal breakpoint is not an exactly representable Float32/64 sample.
   Test each dtype's actual bit patterns adjacent to the exact rational threshold
   with an independent branch oracle. For example Float32 0x3b4d2e1c denotes
   approximately 0.0031308000907301903, lies above the threshold, and takes the
   nonlinear segment; its strict output is 0x3d25aece, not the rounded linear
   anchor 0x3d25aed5. Test negative counterparts and un-clipped values above 1.
3. 709/2020: test beta and its neighbors for each selected coefficient variant.
   Metadata records the explicit variant regardless of dtype or export plan.
4. BT.1886 with Lb=1,Lw=100: E(1)=0, E(100)=1 exactly. Semantic E(0) fails;
   raw E(0) gives -b using the selected exact black/white parameters.
5. PQ: E(10000)=1; E(0)=RN_dtype(c1^m2), approximately 7.309559025784e-7,
   not zero. Negative/above-10000 semantic input fails without clipping.
6. HLG: E(0)=0; E(1) is approximately 0.9999999950661306 before rounding.
   Compare exact input values on either side of 1/12 to the high-precision
   reference; do not pretend rounded input 1/12 is the exact breakpoint.
7. ACEScc: -1 and either zero encode to RN_dtype((-16+9.72)/17.52).
   E(2^-15)=RN_dtype((9.72-15)/17.52). Positive values above 65504 encode
   normally, although a subsequent A cap prevents recovery.
8. ACEScct: E(0)=RN_dtype(B); E(-1)=RN_dtype(B-A). Test Xbreak and neighbors.
   There is no low-end clipping or sRGB-style odd extension.
9. Encoded-source rejection, incompatible reference/unit rejection, explicit
   override behavior and raw metadata retention must be distinct fixtures.
   A P3 group with linear relative values may use sRGB E without changing basis.
10. Alpha-only/AOV-only and disjoint tile-crossing requests copy only requested
    samples. A nonfinite requested color fails even at alpha=0. Forced view of
    a nonidentity curve fails; source state and unchanged bypass samples must
    agree between auto/materialize and full/sparse execution.

A future public workflow explicitly requests the desired encoding after any
needed FMT-10/ref-normalization stages, observes encoded samples, then separately
uses FMT-06 and an I/O codec if external integer/file output is required. This
specification does not implement that workflow or register its proposed keys.
