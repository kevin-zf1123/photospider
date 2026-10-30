---
spec_schema_version: 1
id: KEY-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# KEY: matte generation and color evidence

Inherit [COM common](COM_common_contract.md). Matte values are coverage in [0,1]; known-background flags, trimap labels and confidence have separate meanings. Confidence in this family describes available source support, not a calibrated probability of correctness.

## Scalar features and color keys

KEY-01A is the scalar range primitive. A brightness key is the explicit composition of FMT extraction of linear Y, CIELAB l or OKLab L followed by KEY-01A; it has no separate operation identity. The feature producer fixes units and numerical boundaries. Full CIELAB distance includes 100 times the stored-l difference; chroma-only distance uses a*,b* alone. The two keys are separate operations. Difference RGB uses an aligned clean plate and a distinct RGB metric.

## Evidence and matte

Nearest plate estimation copies original donors, with confidence 1 only at known locations. Temporal median uses explicitly aligned frame inputs and masks; its per-channel median can combine components originating in different frames. A pixel without any valid frame has zero support confidence and no plate-color solution. No motion compensation or inferred frame history is included.

Closed-form matting estimates alpha from an image and trimap using [KEY matting mathematics](KEY_matting_contract.md). The foreground-estimation primitive independently receives image, alpha, background and prior evidence, with an explicit regularizer. Its alpha endpoints are defined copy policies; the interior solves the stated regularized objective. Alpha does not by itself recover missing foreground color.

## Spatial refinement and spill

Erosion/dilation use original matte extrema. Binary hole filling uses four-connected exterior reachability and does not treat a soft matte as a binary mask. Guided refinement has two window stages and support H(2r). Edge color extension changes color while retaining target alpha; its donors remain original image pixels.

Green-excess suppression and chromatic-direction suppression have different formulas. Directional suppression removes only the positive projection along the chromatic spill direction and retains ideal linear Y. Native RGB rounding may alter the recomputed Y slightly. Soft effect masks and strengths are explicit numeric inputs where stated; higher-level mask attachment is composed outside these functions.
