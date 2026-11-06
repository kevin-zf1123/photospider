---
spec_schema_version: 1
id: KEY-05A
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-05
function: closed_form_matting_clipped
execution: tile
proposed_operation_keys:
  - keying.closed_form_matting_clipped
---

# KEY-05A: Trimap-constrained closed-form matting

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.closed_form_matting_clipped`.

## Interface

`image: T[H,W,3]`, `trimap: UInt8[H,W]`, `epsilon: T[1]` -> `matte: T[H,W]`, `raw_residual: Float64[1]`.

Relative linear RGB. Labels 0=background, 1=unknown, 2=foreground. epsilon>0 in squared RGB units, suggested 1e-7. Static solve_atol:Float64=1e-12 and solve_rtol:Float64=1e-8, both >=0 with positive sum.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

Raw selection uses `axis:Int64` and `components:String`. The latter is the comma-separated canonical decimal encoding of the ordered COM component-index list, for example `"0,1,2"`: exactly three distinct in-range indices in RGB or Lab coordinate order. Alternatively, `input_description:String` and `group:String` resolve the selection through the existing FMT description rules; this form excludes `axis` and `components`. A declared spatial channel axis must agree with the resolved selection.

The current implementation resolves an explicit common linear RGB interpretation using `source_basis:String`, or the primaries and white in the selected description group. An explicitly supplied basis must agree with the description. There is no implicit basis and no color conversion; preset names and custom values use the existing FMT basis codec. Description groups use model `rgb`, transfer `linear`, and straight association.

The resolved color sample rank is three; removing the component axis yields `[H,W]`. The component extent is three except for edge extension, which also carries alpha and optional other channels.

`solve_atol:Float64=1e-12` and `solve_rtol:Float64=1e-8` have the stated nonnegative domain and positive sum. `max_iterations:Int64=4096`, in the caller domain >=0, caps the conjugate-gradient fallback only; the dense or banded direct solve and its at most four residual-refinement attempts are separate. A zero cap may succeed if the direct solve or initial iterate already meets the criterion; otherwise failure to meet the criterion is NotConverged.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

Use the clipped 3x3 matting Laplacian, hard constraints and native numerical solution defined in [KEY matting](KEY_matting_contract.md). Clamp solved alpha to [0,1]. Known labels produce exact 0/1. raw_residual is the infinity norm of the unknown-equation residual before clamping.

### Current implementation profile

The implementation assembles the reduced unknown system in Float64 using locally centered RGB values and 3x3 covariance Cholesky whitening. Windows that cannot touch an unknown do not contribute. Systems with at most 256 unknowns first use dense Cholesky with residual refinement. Larger systems whose unknown bounding-box width gives a raster-order half-bandwidth bound of at most 32 first use compact banded Cholesky when its factor fits the declared scratch capacity. Both direct paths retain the original stencil for residual refinement; a failed numerical band candidate restores a finite initial guess. Other or unresolved systems use diagonally preconditioned conjugate gradients. The actual merged matrix infinity norm and a freshly recomputed equation residual determine final acceptance before clamping. Known-only requested matte coordinates can be returned without evaluating the color or epsilon payload.

## Behavior

W support through the image, trimap and epsilon. Matting is a tile-output operation with global mathematical dependence. NoSolution and NotConverged are failure diagnostics as defined in the common contract.

## Acceptance

Fully known trimaps reproduce labels exactly with raw_residual=0. Small partially known fixtures compare to the exact rational system; retain known endpoints even when unknown values need clipping.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
