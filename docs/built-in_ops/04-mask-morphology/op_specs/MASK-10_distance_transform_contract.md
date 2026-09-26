---
spec_schema_version: 1
id: MASK-10
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
---

# MASK-10: Exact discrete nearest-feature and truncated distance fields

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
This family proposes distinct members; it registers no dispatcher or legacy alias.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-10A](MASK-10A_nearest_feature.md) | nearest_feature | Nearest discrete feature and distance |
| [MASK-10B](MASK-10B_signed_center_distance.md) | signed_center_distance | Signed center-to-opposite-class distance |
| [MASK-10C](MASK-10C_truncated_nearest_feature.md) | truncated_nearest_feature | Local truncated nearest-feature distance |
| [MASK-10D](MASK-10D_truncated_signed_distance.md) | truncated_signed_distance | Local truncated signed center-distance |

## Normative shared mathematics and interface

Input Binary[H,W]; required metric l1|l2|linf, sy,sx positive finite Float64,
output_dtype float32|float64; exterior none|background. For unsigned A/C,
feature foreground|background is required. For signed B/D reject feature and
always seek the opposite class at each pixel. Feature centers are integer
coordinates. Under exterior=background, ALL off-canvas lattice sites are
background, none foreground. For in-canvas queries the nearest outside site
can be found on the one-pixel rectangular ring; tie order still includes its
actual negative/out-of-canvas coordinates. None means only D contains sites.

Authoring helpers recommend exterior=none for A/C (nearest-feature distances)
and exterior=background for B/D (signed distances). Both modes are supported
by all four members. Every node must serialize exterior explicitly; evaluation
never infers or switches the mode from image contents. Missing exterior is a
static-parameter error.

L1 distance=abs(dy)*sy+abs(dx)*sx; Linf=max(abs(dy)*sy,abs(dx)*sx);
L2 squared distance=(dy*sy)^2+(dx*sx)^2. Choose the minimum using exact
rational comparisons. Among equal candidates choose lexicographically smallest
GLOBAL (y,x), independent of traversal, threads or tile. This is not a library's
unspecified nearest-site tie. L2 square root is correctly rounded once only
when a numerical distance is requested. `squared: Bool` exists only in A/C and
must be false for L1/Linf; for L2 squared=true rounds squared distance directly.
B/D always return ordinary signed distance, never a signed square.

A outputs `distance:[H,W]` and `nearest:Int64[2,H,W]` (y plane then x).
No sites: distance=+Inf, nearest=(-1,-1). With sites, distance is finite and
coordinates may legitimately be outside D. Do not infer absence from coordinates
alone. There is no valid output. Metadata records feature, metric, spacing,
exterior and the no-feature infinity convention. All requested outputs use the
same selected site and complete input validation.

B outputs `distance` only. For input=1 it is minus distance to background;
for input=0 it is plus distance to foreground. No opposite feature gives -Inf
for foreground, +Inf for background. This is signed CENTER distance: adjacent
0/1 centers yield +1/-1 at unit spacing, not +.5/-.5. No half-pixel correction
or continuous boundary reconstruction is implied. Finite arithmetic overflow is
an error in both A and B, never a no-feature sentinel.

C/D require `limit` finite Float64>0. Search only candidates at true metric
distance<=limit. C returns distance=min(nearest_distance,limit), or its square
when squared=true, and `within:Binary[H,W]`. No feature inside the closed radius:
within=0, distance=RN_T(limit) (or RN_T(limit²)); no coordinates output. Equality
at the limit yields within=1. D seeks the opposite class, returns the same
clamped magnitude with negative foreground/positive background sign and within.
No-feature foreground therefore gets -limit, with no infinity. This is explicitly
a truncated field whose flag means "opposite site within limit", not a full
field existence flag. It does not distinguish distant sites from absent sites. Limit rounding
happens only after exact membership/sign decisions. A squared output can overflow
where an ordinary distance does not; that is a demanded-output arithmetic error.

A/B declare Whole input support and validation for every nonempty requested
output; changing any input may dirty the whole field. C/D declare a finite
metric stencil around Q (plus center classification for D, already in it):
axis bounds floor(limit/sy), floor(limit/sx), then exact metric membership.
Out-of-canvas background candidates are mathematical constants and do not
read pages. Thus finite truncation is a separate local contract, not a claim
that ordinary EDT is halo-local. Empty Q performs no site scan.

Small-image oracle enumerates all sites and compares exact rational distances,
O(N²) worst-case time, O(N) site scratch. Reference implementation candidates
include separable lower-envelope squared EDT (linear arithmetic work on a regular
grid), exact L1/Linf passes and tie-carrying nearest coordinates. Exact predicate
limb costs are additional; no real-RAM complexity claim bounds those bits.
Acceleration must preserve exact site/tie/within decisions, not merely small
distance error. GPU PBA is a candidate; JFA is approximate and not conforming
without exact repair/certification. Versioned approximate fields need their own
error class and downstream restrictions. No nearest-label identity is returned;
compose exact coordinate gather from a separately associated label image.

## Sources, independent evidence and review boundary

[S08](../research-sources.md#s08); [S09](../research-sources.md#s09); [S13](../research-sources.md#s13); [S14](../research-sources.md#s14)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../examples/mask_morphology_oracle/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
