---
spec_schema_version: 1
id: MASK-04F
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-04
function: fit_color_groups_image
oracle_entry: fit_color_groups_image
proposed_operation_keys:
  - mask.fit_color_groups_image_strict
---

# MASK-04F: Fit image regions by group ID

Inherit [MASK-common](MASK_common_contract.md) and the
[family contract](MASK-04_color_range_contract.md). Numerical rules follow
01-numeric. This spec does not claim native registration or production execution.

## Ports, parameters and output inference

image: Float32/Float64 planar[C_image,H,W], group_ids:Int64[H,W],
weights: image-dtype[H,W], epsilon:Float64[C_selected] -> model:Result, skipped:Result.
Required statics: channels (one complete nonperiodic color group) and
input_description. No raw fitting or hidden conversion. Controls match the
image's spatial basis. Unrelated alpha/AOV planes are not mathematical inputs;
static description completeness adds no sample-validation reads.

The admitted sample multiset is exactly the positive-ID pixels in global row-major
order, using selected coordinates and weights. The definition is equivalent to
MASK-04E on this table, but does not require materializing it. complete-input group IDs
are Control; image and weights at positive IDs are Data. The fixed Plan
prepares a finite conservative superset covering all possible admitted samples.
Tile order is not a rounding rule. Empty Result demand is descriptor-only.

## Group statistics and model output

Group ID zero excludes color and weight from mathematical support; every other
ID is positive as an unchecked premise. All positive-ID samples are finite even
with zero weight; weights are finite and nonnegative in the sample dtype. No
production pass validates these premises. For every group use exact W=sum w,
mu=sum(w*x)/W and Sigma=sum(w*(x-mu)*(x-mu)^T)/W, then
Sigma_eff=Sigma+diag(epsilon^2). No rounded mean intermediate or sample correction.
A zero-W group is skipped with diagnostic ID on valid input; violated numerical
premises have no diagnostic or skipped-group guarantee.
No automatic grouping, reassignment, clipping or outlier rejection.

Two named static Result outputs derive from one prepared shared fitting stage.
Use capacity N equal to the input sample count (H*W for the image member),
checked during preparation:

* model: count:Int64[1], ids:Int64[N], mean:Float64[N,C],
  cholesky:Float64[N,C,C]. Effective rows are sorted by positive ID. The upper
  Cholesky triangle is +0. Input color interpretation is explicit configuration.
* skipped: count:Int64[1], ids:Int64[N], with an ascending effective prefix of
  IDs having zero total weight. The reason is zero_total_weight by contract.

Each count may be zero. Unused entries are zero-filled; positive-ID and positive-
diagonal constraints apply only to effective model rows. Both counts are ordinary
payload included in the compiled fixed reads. No absent ID creates an effective row. The complete
schema/codec and fixed publication scopes remain registration gates for these
Proposed members. The global fitting computation is not repeated per output tile.
Source ownership follows the host contract; applying the model to another
compatible image requires no source ObjectId match.

Compute the exact positive-diagonal Cholesky factor of Sigma_eff; emit RN64(mean)
and RN64(each lower factor entry). This is the fitting output rounding boundary.
An exact rational LDL^T plus correctly rounded square roots is a possible
reference. Finite stored entries and a positive representable diagonal are
caller premises, with no output scan or adaptive increase of epsilon. Sample orders and tile partitions cannot alter
these numeric values. epsilon is a required Float64[C] Tensor of positive finite
scales; its units match each native color coordinate. The model represents the
actual stored mean and L, with effective covariance L*L^T.

## Ownership, fixed support and acceptance

For any nonempty fitting Result selection, consume all group IDs, positive-ID
samples/weights, and all epsilon values under their unchecked premises. Lower
this runtime selection to fixed reads covering the static input capacities. Both output schemas
can be inferred without reading group values. Selected semantic groups are
complete; applicable descriptions are checked even when all IDs are zero.
Both Results depend globally on the admitted samples; no tile-local models or
partial tables are published. All-ID-zero gives an empty model and empty skipped
list. Capacity/work limits and cancellation fail without partial publication.
Account for group maps, exact accumulators, factor/refinement state and both
outputs; grouping complexity is not capped by maximum ID. No hidden spill.

The numerical oracle omits actual color descriptors and native Result codecs.
Acceptance includes noncontiguous positive IDs, order invariance, zero weights
with finite colors, ID-zero samples excluded from the formula, empty models,
zero-W diagnostics, single-sample regularization, correlated dimensions,
Float32/64 input, exact LDL^T reconstruction, and matching table/image inputs.
Native public workflows additionally test metadata, resource bounds, immutable
lifetimes, atomic failure and actual source reads. [S27](../research-sources.md#s27)
provides mathematical reference context; all choices above are explicit MASK rules.
Malformed ID/weight/color fixtures exercise oracle diagnostics only.

Oracle entry: `fit_color_groups_image` in [reference.py](../../../../oracle/ops/mask_morphology/mask04/reference.py).
