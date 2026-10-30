---
spec_schema_version: 1
id: FILTER-oracle-protocol
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# Mathematical and quality acceptance

## Evidence classes

| Class | Establishes | Does not establish |
| --- | --- | --- |
| ExactRational | Declared rational expression/stage with direct IEEE rounding | Full special-value coverage |
| DirectedMPFR | Directed interval endpoints round to the same bits | Unbounded completion or all-input certificates |
| HighPrecisionDiagnostic | Approximate high-precision/reference error trends | Strict golden or truncation-error proof |
| ContractFixture | Support/schema/control assertions | Numerical correctness |
| PinnedThirdPartyComparison | Comparison against a fixed third-party implementation, configuration and resource set | Unconditional bit identity or quality ground truth |

Two agreeing precisions do not constitute a certificate. Generated vectors require independent analytic assertions.

## Mathematical acceptance

For E/B/S members, use direct rational-to-binary32/64 rounding and directed MPFR where specified. Keep independent finite-matrix and transform references. For native RES01-12, those references supply ideal mathematical comparisons, while dtype-aware native tests check the implemented stages and invariants. RL tests construct the finite A and its true transpose. Blind PSF tests compare the sorted-threshold projection and native feasibility error. Mean-domain Anscombe inverses require documented truncation/root tolerances and independent range tests.

Fixtures cover both dtypes, signed zeros, subnormal/normal boundaries, halfway cases, overflow to infinity, exceptional arithmetic (payload/priority only where explicitly required), selected and excluded poison values, odd/even shapes, large kernels, singleton axes, nonzero origins and identity branches. Partial finite-domain references report unsupported cases as Inconclusive, not as evidence that legal NUM inputs are invalid.

## Ordinary floating-point transform acceptance

FRQ-01A/B and FRQ-02A/B use F arithmetic. Their independent DFT references are exact or directed mathematical references, but outputs are compared by error, not mandatory bit equality. For finite fixtures without intermediate or output overflow, let epsilon be the output dtype's machine epsilon and K=1+ceil(log2(H*W)). Require relative plane L2 error at most 64*epsilon*K and maximum absolute component error at most 64*epsilon*K*abs(scale)*sum(abs(complex_input)). Test an all-zero input separately without dividing by a zero reference norm. These are fixture acceptance limits, not an all-input error theorem or an OpenCV/MATLAB guarantee. For an RFFT energy comparison, weight interior stored x frequencies twice and DC/Nyquist once.

Include both dtypes and normalization modes, singleton/non-power-of-two/prime axes, independent batches, nonadjacent/reversed axes, selected outputs and independently edited valid inverse spectra. Verify RFFT boundary conjugate pairs and structural imaginary zeros. Nonfinite/range fixtures check ordinary floating-point completion and specified structural behavior, without exact NaN payload, written-order propagation or zero-sign goldens.

## Native RES acceptance

RES01-12 use [RES_native_contract](RES_native_contract.md). Acceptance checks native dtype-aware error and explicit structural/copy behavior; it does not require whole-expression bits or NUM NaN payload priority.

RES01A covers exact rank selection of native residual/deviation samples, odd/even counts, ties, sparse masks with excluded poison, arbitrary spatial axes and independent planes, insufficient selected samples, and the Float64 scale conversion. RES01B covers input precision, native parameter conversion, FMA variance, square root of that native variance, clamps and each output subset.

RES02A/B acceptance covers native Float32/Float64 stages, independent direct-patch and directed-MPFR references, G=1..4, arbitrary spatial axes and independent planes, reflected full-image support, selected ROI/tile closure and s=0 raw copy with no guide/sigma demand. Numerical error measurements state their fixture scope and conditioning assumptions. RES03-12 are subject to their member-specific algorithm/profile acceptance.

## Native algorithms and third-party comparison

BM3D, CBM3D and SMAA 1x are native algorithms with independently pinned third-party comparisons. Record version, build, resource identity/license, input representation, parameters, stages and sampling behavior. Align algorithm steps and explain numerical differences. A manifest or citation is not pixel evidence.

## Image-quality acceptance

Effect-oriented members require mathematical acceptance and quantitative quality scores by use case. Use multiple relevant metrics with each critical threshold met; a weighted total does not hide failure on a critical metric. Reliable references from known synthetic degradation or measured pairs form the primary dataset; unpaired real images supplement it. Third-party output is not ground truth. Metric formulas, datasets, sample aggregation and thresholds are fixed per member; this document fixes the principles only.

Production acceptance supplies valid numerical and association payloads.
