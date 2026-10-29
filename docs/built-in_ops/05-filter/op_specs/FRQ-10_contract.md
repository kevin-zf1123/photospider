---
spec_schema_version: 1
id: FRQ-10
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
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

Inherits the shape, support, resource, cancellation, ownership and publication rules of [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). This family's local F profile defines arithmetic accuracy; the shared NUM specification is unchanged.

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-10A | [haar_mean_lifting_v1 Decomposition](FRQ-10A_decompose_haar_mean_lifting_v1.md) | primitive | F |
| FRQ-10B | [haar_mean_lifting_v1 reconstruction](FRQ-10B_inverse_haar_mean_lifting_v1.md) | primitive | F |
| FRQ-10C | [cdf53_float_lifting_v1 Decomposition](FRQ-10C_decompose_cdf53_float_lifting_v1.md) | primitive | F |
| FRQ-10D | [cdf53_float_lifting_v1 reconstruction](FRQ-10D_inverse_cdf53_float_lifting_v1.md) | primitive | F |
| FRQ-10E | [stationary_haar_mean_v1 Decomposition](FRQ-10E_decompose_stationary_haar_mean_v1.md) | primitive | F |
| FRQ-10F | [stationary_haar_mean_v1 reconstruction](FRQ-10F_inverse_stationary_haar_mean_v1.md) | primitive | F |

Letter suffixes identify the six analysis/synthesis operations. Each member has one registered key and the F profile below. Helpers orchestrate explicit graph stages and preserve the fixed execution interfaces.

### Analysis and Synthesis Profiles
**haar_mean_lifting_v1**: Analyze x first, then y separately for x-low and x-high. Pair e=x[2i], o=x[2i+1], duplicating the last sample for odd lengths. Compute d=o-e and s=e+d/2 in the working dtype. Synthesis uses e=s-d/2, o=d+e, applies y before x and crops each level to its saved original size. All four bands have shape ceil(H/2),ceil(W/2): LL is x-low/y-low, LH_y is x-low/y-high, HL_x is x-high/y-low and HH is x-high/y-high. Recurse only on LL. Low is a mean and detail is a difference, with no orthonormal sqrt(2) scaling. An odd-boundary detail remains editable: synthesis must use its supplied value for retained samples even if unedited analysis produced zero there. General floating-point reconstruction is not bit-exact.

**cdf53_float_lifting_v1**: Use floating-point lifting without integer flooring. Split even samples e of length ceil(N/2) and odd samples o of length floor(N/2). Predict d_i=o_i-(e_i+e_min(i+1,last))/2; update s_i=e_i+(d_max(i-1,0)+d_min(i,last_d))/4. Replicate the terminal even sample for prediction and endpoint detail samples for update. Synthesis first recovers e_i=s_i-(dprev+dnext)/4, then o_i=d_i+(e_i+enext)/2 with the same endpoint rules. Analyze x then y and synthesize y then x. High axes have floor length and low axes ceil length; store every actual band shape. A one-sample 1D axis has identity low and no high. A 2D decomposition stage requires H,W>=2; reject excess levels to avoid zero-length exported bands.

**stationary_haar_mean_v1**: Use wrap boundaries and positive dyadic offsets. At global level l>=1, offset=2^(l-1); each axis computes L(q)=(v(q)+v(q+offset))/2 and H(q)=(v(q)-v(q+offset))/2 in the working dtype. Analyze x then y, retaining original H/W in every band and recursing only on LL. Synthesize y then x using v=L+H; one 2D inverse stage is (LL+LH_y)+(HL_x+HH). Do not average shifted reconstructions. This inverse applies to independently edited coefficient arrays. Check that the offset is representable and no greater than the smallest power of two greater than or equal to max(H,W). Shape and execution budgets bound the global level count.

FilterBands/v1 is a caller-owned static workflow/SDK directory of immutable coefficient references, not an unbounded runtime Result schema. It records the named profile, original and per-band shapes, dtype, global level, identity, role, orientation, sampling step, valid crop, axis order, boundary, phase and reconstruction configuration, with rounding_profile=native_float_v1. Source-grid identity, unit, spacing and origin remain explicit caller-owned interpretation. A direct node publishes or consumes a profile-specific Result with at most 16 tensors for 0..5 levels. SDK helpers compose additional ordinary nodes for deeper transforms and preserve global level metadata; five levels is a per-node grouping limit, not a family-wide cap. Compilation retains requested bands and required predecessors. Intermediate details may be computed for low-band updates or deeper LL levels without exporting unrelated siblings. Whole-plane dependencies, independent tile ownership and complete immutable publication remain unchanged.

Integer-reversible CDF 5/3, Daubechies, Symlet, dual-tree, and complex wavelets are outside this family. Each future addition requires its own named profile and complete specification; an arbitrary wavelet string may not dispatch to a library-defined basis.

## Shared Execution and Numeric Requirements

F is the ordinary native floating-point profile used by this family. Float32 uses binary32 arithmetic; Float64 uses binary64 arithmetic without narrowing. Vectorization, register fusion and explicit FMA are permitted while preserving named formulas, axis order, boundaries, shapes and mathematical dependencies. Correct rounding, per-component ULP bounds and bitwise reproducibility across algorithms or ISAs are not promised. There is no strict/accelerated quality split, precision refinement or strict numerical fallback. NaN, infinity, signed-zero, underflow and overflow results follow the selected native arithmetic and established floating-point environment; numerical premises remain unchecked. Zero-level COPY is an explicit bit-preserving exception.

This local F profile supersedes inherited strict/staged accuracy requirements only for FRQ-10. Formulas and discrete choices remain normative; fusion does not change wavelets, boundary modes, orientation, scale or synthesis rules.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics; do not apply one alpha-blurring rule to all of them. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

The member support set is normative. ROI and tile edges are not image boundaries, and paged access does not remove whole-plane dependencies. Compilation fixes reads and writes for selected computation and necessary intermediates. Numerical and association premises remain unchecked; no separate payload-validation phase is introduced.

All members follow resource, shape, ownership and execution checks in the [common acceptance protocol](FILTER_oracle_protocol.md), with numerical comparisons interpreted under the family F profile. Preserve the mathematical references and self-tests in the [oracle usage guide](../../../../oracle/ops/filter/README.md). State dtype, input range, error norm and tolerance. Passing an oracle self-test or round trip is not complete runtime acceptance. D1/D2 and Proposed describe specification maturity separately from implementation and validation.

## Implementation and Verification Status

All six members are registered under unified `frequency.<member-function>` keys. Haar and stationary Haar use fused SIMD analysis/reconstruction; CDF 5/3 uses vectorized lifting stages and prunes unneeded column branches. Levels=0 uses explicit bit-preserving COPY.

Implementation and registration are complete; this does not establish complete numerical, runtime or performance acceptance.

## Sources and Review

[S16 · PyWavelets signal extension modes](../research-sources.md#s16) ; [S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and registered operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
