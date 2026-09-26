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
Required statics: channels (one complete nonperiodic color group), interpretation
respect|override and conditional source_description. No raw fitting or hidden
conversion. Controls match the image's spatial basis. Unrelated alpha/AOV planes
are not read; effective group-validation closure follows FMT.

The admitted sample multiset is exactly the positive-ID pixels in global row-major
order, using selected coordinates and weights. The definition is equivalent to
MASK-04E on this table, but does not require materializing it. Whole group IDs
are Control/Validation; image and weights at positive IDs are Data/Validation.
Tile order is not a rounding rule. Empty Result demand is descriptor-only.

## Group statistics and model output

Group ID zero excludes color and weight reads; every other ID must be positive.
All positive-ID samples are validated even with weight zero. Weights are finite
and nonnegative in the sample dtype. For every group use exact W=sum w,
mu=sum(w*x)/W and Sigma=sum(w*(x-mu)*(x-mu)^T)/W, then
Sigma_eff=Sigma+diag(epsilon^2). No rounded mean intermediate or sample correction.
A zero-W group is skipped with diagnostic ID; validation failure is not a skip.
No automatic grouping, reassignment, clipping or outlier rejection.

Two named complete Result outputs are published together:

* model: RuntimeCount=G effective groups; ids:Int64[G], mean:Float64[G,C],
  cholesky:Float64[G,C,C]. Static schema includes C and the effective ordered
  color-coordinate description. The upper triangle is +0. Rows are sorted by ID.
* skipped: RuntimeCount=S; ids:Int64[S], ascending IDs with zero total weight;
  the schema fixes reason=zero_total_weight. No invented rows for absent IDs.

Each output has its own RuntimeCount; G or S may be zero. These are Result fields,
not zero-extent ordinary Values. Exact runtime schema/metadata codecs must be
implemented and reviewed before native registration; names above are not new
working facet IDs. Model and diagnostic publication is complete, without partial
successful rows. Source input ownership is retained as required by the host,
while application to another compatible image requires no source ObjectId match.

Compute the exact positive-diagonal Cholesky factor of Sigma_eff; emit RN64(mean)
and RN64(each lower factor entry). This is the fitting output rounding boundary.
An exact rational LDL^T plus correctly rounded square roots is a possible
reference. Finite overflow or loss of a positive representable diagonal fails;
no adaptive increase of epsilon. Sample orders and tile partitions cannot alter
these numeric values. epsilon is a required Float64[C] Value of positive finite
scales; its units match each native color coordinate. The model represents the
actual stored mean and L, with effective covariance L*L^T.

## Ownership, validation and acceptance

For any nonempty fitting Result request, validate all group IDs and consume only
positive-ID sample/weight payloads, plus all epsilon values. Both output schemas
can be inferred without reading group values. Selected semantic groups are
complete; applicable descriptions are checked even when all IDs are zero.
Both Results depend globally on the admitted samples; no tile-local models or
partial tables are published. All-ID-zero gives an empty model and empty skipped
list. Capacity/work limits and cancellation fail without partial publication.
Account for group maps, exact accumulators, factor/refinement state and both
outputs; grouping complexity is not capped by maximum ID. No hidden spill.

The numerical oracle omits actual color descriptors and native Result codecs.
Acceptance includes noncontiguous IDs, order invariance, zero weights with
nonfinite colors (error), ID-zero nonfinite payloads (unconsumed), empty models,
zero-W diagnostics, single-sample regularization, correlated dimensions,
Float32/64 input, exact LDL^T reconstruction, and matching table/image inputs.
Native public workflows additionally test metadata, resource bounds, immutable
lifetimes, atomic failure and actual source reads. [S27](../research-sources.md#s27)
provides mathematical reference context; all choices above are explicit MASK rules.

Oracle entry: `fit_color_groups_image` in [reference.py](../../../../examples/mask_morphology_oracle/reference.py).
