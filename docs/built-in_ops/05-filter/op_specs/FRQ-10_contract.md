---
spec_schema_version: 1
id: FRQ-10
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
parent_id: 05-filter
members:
- FRQ-10A
- FRQ-10B
- FRQ-10C
- FRQ-10D
- FRQ-10E
- FRQ-10F
research_sources:
- S16
- S17
---

# FRQ-10: Named Wavelet Analysis and Reconstruction

This family defines exactly three named profiles: `haar_mean_lifting_v1`, `cdf53_float_lifting_v1`, and `stationary_haar_mean_v1`. It does not claim compatibility with arbitrary PyWavelets bases. Together they define six members: one decomposition and one inverse for each profile.

Inherits [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-10A | [haar_mean_lifting_v1 Decomposition](FRQ-10A_decompose_haar_mean_lifting_v1.md) | primitive | S |
| FRQ-10B | [haar_mean_lifting_v1 reconstruction](FRQ-10B_inverse_haar_mean_lifting_v1.md) | primitive | S |
| FRQ-10C | [cdf53_float_lifting_v1 Decomposition](FRQ-10C_decompose_cdf53_float_lifting_v1.md) | primitive | S |
| FRQ-10D | [cdf53_float_lifting_v1 reconstruction](FRQ-10D_inverse_cdf53_float_lifting_v1.md) | primitive | S |
| FRQ-10E | [stationary_haar_mean_v1 Decomposition](FRQ-10E_decompose_stationary_haar_mean_v1.md) | primitive | S |
| FRQ-10F | [stationary_haar_mean_v1 reconstruction](FRQ-10F_inverse_stationary_haar_mean_v1.md) | primitive | S |

Letter suffixes identify distinct mathematical or interface objects, not quality tiers that a backend may choose arbitrarily. Primitives may propose separately named strict and accelerated variants. Helpers only orchestrate explicit stages. An external engine requires fixed resources and approval before it can have an executable configuration.

## Shared Mathematics, Units, and Profiles


### Analysis and Synthesis Profiles
**haar_mean_lifting_v1**: Apply the 1D transform along x first, then along y separately for x-low and x-high. Pair e=x[2i], o=x[2i+1]; for odd lengths, set the final o=e (duplicate-last). Compute d=RN_t(o-e), s=RN_t(e+d/2). Inverse: e=RN_t(s-d/2), o=RN_t(d+e), then crop to the saved original length. Each level’s four bands have size ceil(H/2),ceil(W/2): LL is x-low/y-low, LH_y is x-low/y-high, HL_x is x-high/y-low, and HH is x-high/y-high. Recursively decompose only LL at each level and publish the detail bands and final LL. Detail is a difference and low is a mean, not an orthonormal sqrt(2) scaling. Floating-point lifting does not claim bit-perfect reconstruction.

**cdf53_float_lifting_v1**: This is not integer-floor CDF 5/3. Split even e with length ceil(N/2) and odd o with length floor(N/2). For N=1, the low band is identity, there is no high band, and that axis is not decomposed further. Predict d_i=RN_t(o_i-(e_i+e_min(i+1,last))/2); update s_i=RN_t(e_i+(d_max(i-1,0)+d_min(i,last_d))/4), replicating endpoint details. Inverse first computes e_i=RN_t(s_i-(dprev+dnext)/4), then o_i=RN_t(d_i+(e_i+enext)/2). Apply x then y in 2D and reverse the order for inverse. Each high-axis band has floor length, so each level requires H,W>=2; otherwise stop and reject excess levels to avoid zero-length bands. Store each band’s actual ceil/floor shape.

**stationary_haar_mean_v1**: Use wrap boundaries. At level l, offset=2^(l-1); on each axis compute L(q)=RN_t((v(q)+v(q+offset))/2) and H(q)=RN_t((v(q)-v(q+offset))/2). Apply x then y; all four bands retain the original H/W. Inverse each axis with v=RN_t(L+H), applying y then x; do not additionally average multiple shifted inverses. Each level recursively decomposes LL and publishes three detail bands plus the final LL. This is a defined redundant filter bank, not the normalization used by every SWT library. Check offset arithmetic; levels must keep the checked offset representable and no greater than the next power of two above max(H,W). Execution budgets also bound admission, without an arbitrary fixed level cap.

Every profile defines RN_t at each pair and axis stage; fusion may not change rounding. FilterBands/v1 records the profile, original shape, level shapes, band identities, roles, orientations, axis order, boundaries, phase, dtype, and all reconstruction metadata. Descriptors for every band are static and complete without computing coefficients. Requests materialize only the requested band and required predecessor bands, not all siblings. Lazy execution is guaranteed at band granularity; within-band Region locality is unspecified unless a member explicitly guarantees it. Every requested band is a full plane unless local computation is explicitly guaranteed. Coefficient edits produce a new immutable collection. Any producer may supply a collection whose complete descriptors and reconstruction metadata are valid; producer identity is not required. All payload access and materialization follows the shared collection and resource contracts.

Integer-reversible CDF 5/3, Daubechies, Symlet, dual-tree, and complex wavelets are outside this family. Each future addition requires its own named profile and complete specification; an arbitrary wavelet string may not dispatch to a library-defined basis.


## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics; do not apply one alpha-blurring rule to all of them. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines actual dependencies and dirty propagation.

## Dependencies and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently; paged access does not remove a whole-image dependency. Follow each member definition for staged rounding, global connectivity, transforms, and iterative state. Independently requested outputs must not turn validation of an unrequested component payload into a hidden whole-image dependency.

All members must follow the [common acceptance protocol](FILTER_oracle_protocol.md). See the [oracle usage guide](../../../../oracle/ops/filter/README.md) for mathematical references and associated self-tests. No new implementation was run in the Photospider runtime. D1/D2 indicate draft maturity, not acceptance rates.

## Sources and Review

[S16 · PyWavelets signal extension modes](../research-sources.md#s16) ; [S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
