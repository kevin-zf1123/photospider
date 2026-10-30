---
spec_schema_version: 1
id: RES-04
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: 05-filter
members:
- RES-04A
- RES-04B
---

# RES-04: Native BM3D/CBM3D

The `haar_opponent_v1` profile defines two-stage block matching, orthonormal Haar collaborative filtering and Kaiser-weighted aggregation. RES-04A processes one numerical plane; RES-04B applies the same stages to fixed opponent coordinates of an opaque RGB image.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-04A | [Native grayscale BM3D profile](RES-04A_bm3d_native.md) | primitive | F |
| RES-04B | [Native color CBM3D profile](RES-04B_cbm3d_native.md) | primitive | F |

Each member has one operation key for both Float32 and Float64. Its SDK helper appends one primitive node; that node computes the hard-threshold basic estimate and the pilot-guided Wiener estimate.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Shared parameters

All algorithm parameters are static and required, with no defaults. The runtime `sigma` input is a Float64 tensor of shape `[1]`, in input sample units. Real parameters and sigma convert to the input dtype before arithmetic.

| Parameter | Type | Domain and meaning |
| --- | --- | --- |
| `y_axis`, `x_axis` | Int64 | Distinct nonnegative axes within the input rank. When spatial layout metadata is present, they equal its height and width axes. |
| `profile` | String | Exactly `haar_opponent_v1`. |
| `block_size` | Int64 | B in {1, 2, 4, 8, 16, 32}; square patch side in pixels. |
| `search_radius` | Int64 | R >= 0; candidate-center offsets cover the inclusive square [-R,R] on both axes. |
| `group_size` | Int64 | G in {1, 2, 4, 8, 16, 32, 64}; maximum retained group size. |
| `reference_step` | Int64 | Integer s in [1,B]; spacing between reference centers. |
| `match_hard` | Float64 | Finite nonnegative strict upper bound on mean squared patch distance in the hard stage, in squared matching-coordinate units. |
| `match_wiener` | Float64 | The corresponding bound on distances in the basic estimate for the Wiener stage. |
| `hard_threshold` | Float64 | Finite nonnegative multiplier lambda for the hard cutoff lambda*sigma. |
| `kaiser_beta` | Float64 | Finite nonnegative beta for the separable Kaiser window; zero gives a uniform window. |
| `max_work` | Int64, optional | Nonnegative operation work limit, default `INT64_MAX`; direct operation nodes may supply it. The SDK helpers use the default. |

Structural shape, profile, axis, block/group size and step constraints are checked. Finite nonnegative sigma, real-parameter domains, working-dtype representability and finite participating comparison values/intermediates are unchecked caller premises. Capacity or work-limit exhaustion returns `ResourceExhausted`; cancellation propagates `Cancelled`.

## Mathematics: haar_opponent_v1

### Coordinates, extension and grouping

For an axis of length n, reference centers are `0,s,2s,...` less than n, followed by n-1 when it is not already present. Visit centers in y-then-x order. A B by B patch at (y,x) starts at `(y-floor(B/2),x-floor(B/2))`.

Extend the complete logical image by reflect101. For n>1, reduce an integer coordinate modulo 2(n-1) into [0,2(n-1)), then map q to q if q<n and to 2(n-1)-q otherwise. A singleton axis repeats its sole sample. Candidate centers may lie outside the original image.

The matching guide is the input in the hard stage and the basic estimate in the Wiener stage. RES-04B uses only its first opponent coordinate for matching. For each reference, include self first. For every other center in its radius square, form the native sum of squared differences over B squared samples, clamp a negative rounded sum to zero, and accept it when that distance is strictly less than `match * B * B`. Sort accepted candidates by `(native squared distance,y,x)`, retain at most G-1 after self, and truncate the resulting list to its largest power-of-two prefix. Equality to the matching threshold is rejected; equal computed distances use coordinate order. The two stages match independently.

### Transforms and shrinkage

Apply the recursive orthonormal Haar analysis to each patch row and then each column. Each level maps adjacent a,b to `(a+b)/sqrt(2)` and `(a-b)/sqrt(2)`, places low coefficients before high coefficients, and recursively transforms the low prefix. Apply the same analysis along the ordered group axis for each spatial coefficient. Synthesis reverses group analysis, then patch columns and rows.

In the hard stage, retain each coefficient z when `abs(z) >= lambda*sigma`; otherwise set it to zero. This rule includes the DC coefficient. For each channel and group, let K be the number of nonzero retained coefficients and use weight `1/max(K,1)`. Reconstruct and aggregate to obtain the basic estimate.

In the Wiener stage, transform both the original input patches and the basic-estimate patches at the newly selected centers. For each original coefficient z and basic coefficient b, use

$$
g=\frac{b^2}{b^2+\sigma^2},\qquad z'=gz.
$$

The group weight for each channel is `1/sum(g*g)`, or one when that sum is zero. A scaled evaluation of the gain may avoid squaring underflow/overflow under the native arithmetic contract. The common sigma-to-the-minus-two aggregation factor cancels during normalization and is omitted in both stages.

### Aggregation

Let I0 be the modified Bessel function of order zero. For B>1 define

$$
k_i=\frac{I_0\!\left(\beta\sqrt{\max(0,1-(2i/(B-1)-1)^2)}\right)}{I_0(\beta)},\quad 0\le i<B.
$$

For B=1, k0=1. The patch window is `k_y*k_x`. Multiply it by the group weight, accumulate the weighted reconstructed samples and weights at their original coordinates, and divide the accumulated numerator by denominator. Crop each reconstructed patch to the actual image during accumulation. Process reference groups in y,x order and patches in their retained group order. Each stage uses its own accumulation and normalization.

Native FMA and reduction regrouping are allowed by [RES native arithmetic](RES_native_contract.md). Group selection uses the computed native distances; mathematically equal or nearly equal distances can round differently between permitted evaluations, changing selected patches and the final output. Exact tie rules apply to equal computed values.

## Behavior and Acceptance

The support is the complete logical plane or RGB group. A requested ROI or tile uses that complete domain for matching, extension and both stages; tile edges are not image boundaries. Output shape, dtype, layout and sample ordering match the input. Sigma=0 copies input bits and skips both denoising stages and numerical-domain validation.

Acceptance covers both dtypes, singleton and non-square images, every supported block/group size, final reference centers, strict matching and hard-threshold equality, coordinate ties, whole/ROI/tile consistency and sigma=0 bit preservation. For RES-04A with B=G=1, R=0 and beta=0, each scalar a has basic estimate b=a when `abs(a)>=lambda*sigma`, otherwise b=0, and final value `a*b*b/(b*b+sigma*sigma)` for positive sigma.

Compare each dtype with an independent staged-native reference and a higher-precision mathematical reference. Check selected coordinates separately against the computed native distances. For differing near-tie groups, compare each subsequent transform/filter/aggregation path using its own independently checked groups; this isolated comparison does not validate matching. Report input scale, dimensions, conditioning and justified member tolerances under the [acceptance protocol](FILTER_oracle_protocol.md). A shared algorithm name or similarly named parameters do not establish pixel equivalence with another BM3D profile.

## References

[S20 · BM3D author project](../research-sources.md#s20)
