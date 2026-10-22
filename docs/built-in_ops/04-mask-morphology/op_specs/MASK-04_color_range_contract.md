---
spec_schema_version: 1
id: MASK-04
kind: operator_family
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---

# MASK-04: Color and coordinate selectors

Inherit the complete [MASK baseline](MASK_common_contract.md), including its
precision, descriptor, error, demand, ownership and acceptance obligations.
All members in this Proposed family are registered in the bundled strict CPU
set.

## Members

| ID | Function | Purpose |
| --- | --- | --- |
| [MASK-04A](MASK-04A_coordinate_range.md) | coordinate_range | Scaled coordinate-distance mask |
| [MASK-04B](MASK-04B_lab76_range.md) | lab76_range | Normalized-Lab DeltaE76 selector |
| [MASK-04C](MASK-04C_hue_range.md) | hue_range | Periodic hue selector with explicit neutral policy |
| [MASK-04D](MASK-04D_lab2000_range.md) | lab2000_range | CIEDE2000 color range |
| [MASK-04E](MASK-04E_fit_color_groups_table.md) | fit_color_groups_table | Fit grouped color samples |
| [MASK-04F](MASK-04F_fit_color_groups_image.md) | fit_color_groups_image | Fit image regions by group ID |
| [MASK-04G](MASK-04G_apply_color_groups.md) | apply_color_groups | Apply grouped color selection |

## Normative shared mathematics and interface

The initial scope includes A-C below, a separately named CIEDE2000 Lab selector,
and statistics-assisted color selection with explicit fitting and application.
D specifies CIEDE2000; E/F specify grouped fitting and G its application. Statistics-assisted selection models multiple color clusters rather
than collapsing all samples to a single center/distribution. Fitting consumes
explicit samples and produces a reusable multi-cluster model; application
consumes that model and an image and explicitly combines cluster responses.
The caller supplies explicit sample group IDs. Each group is fitted independently
with a mean and full covariance; samples are not automatically reassigned.
No K-means, EM, random initialization, automatic group-count selection or hidden
outlier rejection is performed. Application evaluates each group's Mahalanobis
distance under an explicitly specified response-combination rule. Fitting
requires an explicit positive finite per-channel regularization scale epsilon,
in the corresponding sample coordinate units. Every group uses
Sigma_eff = Sigma + diag(epsilon_j^2), including nondegenerate groups; there is
no conditional fallback, inferred epsilon, eigenvalue clipping or pseudoinverse.
This retains off-diagonal covariance terms and gives a positive-definite exact
matrix for a valid nonempty group with positive total weight. Squaring and
addition follow the specified NUM rounding boundaries; an implementation must
not silently lose regularization by premature underflow. The fitted-model
representation is constructed from the positive-definite matrix required by
application; representability of its stored positive diagonal is a caller premise. This rule does not define a mean for an empty group.
Two separately named fitting entry points are provided. The table entry consumes
samples:Float32/Float64[N,C] and group_ids:Int64[N]. This is a generic numeric
sample table, not interleaved image storage. The image entry consumes a planar
image:Float32/Float64[C,H,W] and group_ids:Int64[H,W] on the same spatial basis.
It can accumulate statistics directly without materializing a sample table.
Both use identical sample-coordinate interpretation, mean, full covariance and
regularization rules. Equivalent admitted samples with the same multiplicities
and weights must produce the same model under the NUM contract. Sample-order
or tile-order changes must not introduce a different rounding convention.
Both fitting entry points require explicit weights in the sample floating dtype:
weights[N] for the table and weights[H,W] for the image. Consumed weights must be
finite and nonnegative; values above one are legal. Weights express relative
fitting contribution, not membership probabilities or output coverage. Equal
weighting is authored as all ones, not an omitted input. Weight accumulation
and arithmetic follow NUM; negative/NaN/Inf weights are invalid and finite
overflow is not silently clamped. For each group with W=sum_i w_i>0, define
mu=sum_i(w_i*x_i)/W and
Sigma=sum_i(w_i*(x_i-mu)*(x_i-mu)^T)/W. There is no degrees-of-freedom correction
and no frequency-weight interpretation. Add diag(epsilon_j^2) after this
population covariance definition. A single positive-weight sample has zero
unregularized covariance. Exact common positive scaling of a group's weights
leaves its mathematical mean and covariance unchanged; storage rounding of
newly supplied weights is not assumed to preserve that exact scaling.
These expressions use NUM rounding at the eventual member-defined output/stage
boundaries, without an implicit rounded-mean intermediate in the covariance.
Both fitting entries use Int64 group IDs: zero excludes the sample, positive
IDs identify groups, and negative IDs are invalid. IDs need not be contiguous.
Models are ordered by ascending positive ID, never by first encounter or hash
iteration. The complete group-ID domain is an unchecked caller premise.
At ID zero, color samples and weights are not mathematical Data;
their descriptors are still checked. A positive-ID sample with zero weight is
distinct from an excluded sample. No artificial group zero is emitted.
For a positive-ID group with exactly zero total weight, emit no fitted model
row and include its ID in skipped_group_ids, sorted ascending. The diagnostic
reason is zero_total_weight. Diagnostics enumerate only positive IDs present
in the input, not missing integers between IDs. Illegal weights are outside
the caller premise and have no diagnostic or skipped-group guarantee. Effective model rows contain only groups with
positive total weight. If all IDs are zero or all present groups have zero
weight, return an empty model with payload count=0 with unchanged static capacity. Applying an empty model
produces an all-zero Coverage mask; there is no inferred fallback group.
Application combines effective group responses by maximum:
coverage(p)=max_g response_g(p), with the empty maximum defined as +0.
Each response is in [0,1]. There are no mixture priors, group-weighted averages,
additive accumulation or independent-coverage union in this member. Repeating
an identical group response does not strengthen the output. Group sample counts
and total fitting weights do not become application blend weights.
Application requires one shared set of finite Float64 inner,outer with
0<=inner<=outer and curve linear|smoothstep. These parameters apply to every
effective group in Mahalanobis-distance units. They are application parameters,
not fitting/model fields; there is no per-group override table. For inner=outer,
response_g=[d_g<=inner]. Otherwise response is 1 at d_g<=inner, 0 at d_g>=outer,
and 1-F((d_g-inner)/(outer-inner)) between the bounds, using the chosen curve.
Every node serializes these fields explicitly.

Each effective group stores its mean and lower-triangular Cholesky factor in
Float64, independently of input sample dtype. Compute the exact mathematical
population covariance and regularization first, then the unique lower factor L
with positive diagonal satisfying Sigma_eff=L*L^T. The fitting output is an
explicit NUM rounding boundary: store RN64(mu_j) and RN64(L_jk) for k<=j;
upper-triangular entries are +0. Do not first round mu or Sigma_eff and factor
that different matrix. Required stored entries must be finite and every stored
diagonal must be positive. These representability conditions are caller
premises; execution adds no output validator and does not change epsilon, clip
eigenvalues or use a pseudoinverse.

Application treats the stored mean and factor as exact binary inputs. Define
z by the mathematical triangular solve L_stored*z=x-mu_stored and
d=sqrt(sum_j z_j^2). The effective covariance of the exported model is therefore
L_stored*L_stored^T; it is not claimed to equal an independently rounded full
covariance. The solve introduces no unlisted Float64 per-step rounding. Distance
threshold predicates and final response rounding follow NUM and the member's
explicit boundaries. Static preparation checks model schema/dimensions and the
explicit coordinate description. Finite entries, triangular form and positive
diagonal are unchecked payload premises. Coverage output uses the image dtype.

For every positive group ID, fitting consumes the color coordinates and weight,
including when the weight is exactly zero. Color coordinates are finite and
consistent with the explicit description; weights are finite and nonnegative.
These are unchecked premises. A zero coefficient contributes zero without
removing the sample from the declared support. Only ID-zero samples are excluded
from mathematical color/weight support; the compiler still prepares a finite
conservative tile superset for this runtime selection. Zero-total-weight groups
are an ordinary algorithmic outcome on valid input, with the specified skipped ID.

Statistical fitting and application require caller-owned `input_description`
configuration for one complete nonperiodic FMT color-coordinate group. Raw
arbitrary-feature mode is not offered. The workflow pairs the model Result with
this explicit interpretation and passes it to each consumer; the Result carries
no attached description. Application uses the same coordinate interpretation
after any explicit upstream conversion, as a caller premise. Equal component
counts alone do not establish that premise. Coordinates and epsilon use native
FMT scales, including CIELAB l=L*/100. No color conversion, rescaling, channel
permutation or interpretation is inferred from samples. Static preparation
checks the supplied description and selectors against the schemas. Periodic hue
coordinates are not covariance dimensions. Unrelated alpha/AOV channels are not
fitting data. Table columns carry the same ordered group interpretation in the
caller's configuration. Models can be applied to other compatible images and
are not tied to a source-image ObjectId. CIEDE2000 does not replace DeltaE76.
All seven members are registered. The operations SDK supplies version-1
fixed-capacity model/skipped schemas and the CPU lowering emits fixed reads and
complete publication scopes.

The CIEDE2000 selector requires input_description for one complete native
CIELAB group, with no raw mode. It also requires kL,kC,kH as explicit positive finite Float64
static parameters. Authoring helpers recommend 1,1,1 and serialize them; no
image-dependent factor selection or hidden default is allowed. They are the
parametric factors of the complete CIEDE2000 expression, distinct from the
inner/outer response thresholds. Preserve the complete formula including its
chroma-hue cross term. Unit and non-unit factor cases, zero chroma and angular
branch boundaries require independent acceptance. Use native FMT CIELAB
l=L*/100 with exact 100*l interpretation before evaluating the formula, without
an extra rounded conversion stage. Sample and target descriptions must have
compatible Lab interpretation. Numerical rounding inherits NUM; published
decimal check values alone do not certify correctly rounded final outputs.

For A-C, input is planar Field[C,H,W]. Required `channels` is an ordered unique canonical
String. A uses any K>=1 selected coordinates, `target: Float64[K]` and
`scales: Float64[K]` required Tensor ports; B uses K=3, `target: Float64[3]`;
C uses two selected raw coordinates hue and chroma, no target Tensor.
Runtime targets/scales are controls read for nonempty output selection; their
finite/positive domains are unchecked premises. They are not executed during
output schema inference. Output Coverage[H,W] uses the image dtype. Optional
`input_description` selects the semantic form; its absence selects the documented
raw form. There is no respect/override or separate interpretation mode. A raw
coordinate metric assigns no perceptual meaning. Semantic descriptions and
selectors are checked structurally during preparation; their completeness adds
no payload validation closure. Targets use the same native coordinate convention
and compatible interpretation. Scales are positive finite coordinate-unit scales,
not a color profile. Unselected alpha/AOV samples are not mathematical inputs.

A distance is sqrt(sum_j(((c_j-t_j)/s_j)^2)). B requires CIELAB l,a*,b* and
uses sqrt((100*(l-lt))^2+(a-at)^2+(b-bt)^2); never first round 100*l.
Thus l=.50 vs .51 with equal a,b is about DeltaE76=1 for stored inputs,
not .01. B raw still explicitly means these three normalized-Lab coordinates.
C is an explicit hue/chroma coordinate primitive: raw selection or a recognized
polar group whose angle has first been explicitly converted to turns. Only
hue/chroma enter mathematical support; the complete static group description
does not add reads of other color coordinates. Let h0 be center_turns and delta=(h-h0)-floor(h-h0+1/2).
Hue distance is abs(delta) in [0,.5]; arbitrary finite winding is preserved at
input and reduced exactly for this comparison. chroma=0 or
chroma<minimum_chroma uses explicit neutral_policy include|exclude: include
returns 1 and exclude returns +0 without a hue-distance decision. Positive
chroma equal to minimum_chroma uses the hue branch. Authoring recommends exclude;
the node serializes both fields. Finite consumed coordinates and nonnegative
chroma are caller premises, without sample or complete-group validation. No RGB->HSL hidden transform.

A-D and G use required `inner,outer` finite Float64 with 0<=inner<=outer
(C also outer<=.5), plus curve linear|smoothstep. A/B use distance units above;
C uses turns. For inner=outer, output `[distance<=inner]`. Otherwise output 1
at distance<=inner, 0 at distance>=outer, and 1-F((distance-inner)/(outer-inner))
in between. Exact branch comparisons can compare squared distances before any
sqrt. Certified irrational evaluation is needed only in an interior ramp.

For A-C, CPU cost is O(|Q|K), with O(K) exact/control scratch plus refinement.
These three members have no complete-input statistics or inferred selection center.
Statistics-assisted selection uses explicitly supplied samples in a separate
fitting stage; applying its fitted model is separately specified. Neither that
model nor CIEDE2000 changes A-C's formulas or demand rules. All numerical
rounding and accuracy requirements inherit 01-numeric.

## Sources, independent evidence and review boundary

[S03](../research-sources.md#s03); [S04](../research-sources.md#s04)

The reference material supports the explicitly cited concept, not every project
choice in this draft. Member examples and the [oracle suite](../../../../oracle/ops/mask_morphology/README.md)
are the acceptance starting point. Proposed scope does not relax the inherited
NUM numerical standard.
