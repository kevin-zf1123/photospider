---
spec_schema_version: 1
id: GRD-common
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD: shared grading contract

This contract defines the shared inputs, units, arithmetic and spatial behavior of the grading operators. Member specifications override a family rule only where they state the exception. The [foundation contract](../../00-foundation/contracts.md) supplies structural validation and unchecked caller premises.

## Interface conventions

Every member is an independently callable tile operator with a proposed key. Each ordered input port is a Result containing exactly one tensor at index zero; the input tensor key is unconstrained. Each ordered output port is a `photospider.tensor` version 1 Result with one tensor named `samples`. Output names are those in the member signature. Shapes and dtypes are inferred from input descriptors and static parameters.

`T` is Float32 or Float64. All image inputs within a signature marked T share that dtype; image outputs retain T. `S` denotes the ordered non-component sample axes; `[S,C]` is logical notation for a component axis, not a storage-layout requirement. `axis:Int64` identifies that axis; ordered `components:Array[Int64]` identifies selected slots. A supplied `input_description` may resolve these selectors under the existing FMT structural rules. Model-dependent formulas require their explicit model, RGB basis, white, transfer, reference and units. They use the existing FMT description representation. Descriptions are static call parameters; Results carry no color metadata.

Rank is 1..8, stored extents are positive, and the logical element count is at most 2^40. Scalar ports have shape [1]. A member with explicit `[H,W]` or `[H,W,C]` processes one two-dimensional image, with logical y,x order and the declared component axis. Global integer origin defaults to (0,0), is supplied as `origin_x/origin_y:Int64` when coordinates are observable, and remains fixed across ROI requests. Screen/dither origin parameters describe pattern phase in this coordinate system. Batch repetition of a spatial member is an explicit outer composition.

Names in an ordered port signature are required dynamic inputs. Values described as “suggested” are values for the caller to bind, not optional ports. Parameters outside the signature are static; enum strings, Bool, Int64, Float64 and fixed arrays have the types shown. A numeric static scalar without an explicit type is Float64; sizes/radii/counts are Int64. Parameters without a stated default are required. Static arrays use the displayed shape. An image or control field matches its declared axes exactly; broadcasting is explicit upstream.

Same-shape members replace only selected components. Alpha and unrelated AOVs copy bit-for-bit, including NaN payloads and signed zero. They have independent sample dependencies. Shape-changing members state whether these slots are removed or preserved. A new single-feature output contains that feature alone. Alpha is straight and, when present, is an explicitly selected plane inside the source image. Four channels alone do not identify alpha. Hidden color at alpha=0 participates in ordinary grading; statistical alpha exclusion is an explicit separate policy.

## Native numerical computation

GRD uses ordinary FP32/FP64 arithmetic. Algebraic expressions describe the target function; production execution does not require exact-real intermediates or correctly rounded whole expressions. Calculation dtype is Float64 when a consumed arithmetic operand is Float64, otherwise T. Static floating controls are Float64. Sums, moments, histogram weights, feature extraction and explicitly named internal color conversions use Float64. The output is converted once from its calculation value to the declared output dtype. Integer counts and returned flags retain their discrete definitions.

Consumed numerical controls, lookup tables and color coordinates are finite and satisfy the stated member domains as unchecked caller premises. Static structural/type checks follow the foundation. Reject-domain modes declare a caller domain; they do not require a runtime payload scan or error. Copy branches preserve source bits. Active floating overflow follows IEEE signed infinity; invalid arithmetic produces NaN. There is no automatic replacement by finite extremes. These generated nonfinite values may be outside a downstream color consumer's premise. Gradual underflow is the arithmetic reference; no extra signed-zero canonicalization is imposed on ordinary arithmetic.

Sums have a canonical logical order: ascending component/index for vectors and tables, row-major full-image coordinates for spatial reductions. The reference is a left fold. Equivalent reassociation within continuous numerical tolerances is permitted; it must not make results depend on ROI or tile partition. Recurrences and quantization retain their explicit visit order, selected indices and tie rules. Fused arithmetic and ordinary library transcendentals are allowed within numerical acceptance. Algorithms that change the mathematical filter, statistic or interpolation are separate members.

The [strict oracle](../../../../oracle/ops/07-grade/README.md) evaluates rational formulas exactly and transcendental expressions with certified directed enclosures, then rounds to the declared dtype using nearest/ties-to-even. Explicit feature/table/output stage boundaries also round. It is the comparison reference for native execution, not a demand that production use arbitrary precision. Stateful recurrences have the intermediate Float64 boundaries specified by the member; the oracle rounds those steps strictly. At a discrete decision boundary, record the reference decision and the native decision separately; approximate color error cannot establish equality of masks, palette indices or quantized patterns.

For finite continuous outputs, the default analytic-fixture acceptance is abs(error)<=atol+rtol*abs(reference), with (atol,rtol)=(2e-6,2e-5) for Float32 and (2e-12,2e-11) for Float64 in native coordinate units. Large reductions use an additional absolute term `8*n*eps64*sum(abs(terms))` for n summed terms. These are comparison criteria for the declared fixtures, not universal conditioning guarantees for arbitrary extreme input. Identities and bypasses specified as copies compare bits; integer counts, states, mask endpoints and selected palette entries compare exactly. Decision-boundary fixtures compare using the specified native intermediate sequence as well as reporting the strict reference; no general floating tolerance is applied to a discrete label.

## Color meaning

The [FMT color model definitions](../../02-format-color/op_specs/FMT-11_model_math.md) and [RGB basis definitions](../../02-format-color/op_specs/FMT-10_basis_math.md) supply equations, matrix coefficients, scales and model domains. GRD evaluates those equations under the native arithmetic above; it does not inherit FMT strict rounding or accelerated-profile requirements. An independently invoked FMT node retains its own contract.

CIELAB/CIELCh use l=L*/100 and native a*,b*,C* units. OKLab/OKLCh use their native units. Hue is explicitly radian or pi_multiple; only periodic lookup reduces winding. A bounded HSL/HSV member clamps only the coordinates specified there. General signed/HDR RGB, Lab and affine outputs remain unclipped. Gamut mapping, transfer conversion, effect-strength mixing and soft effect masks are separate explicit operations. The coefficient limit or sample domain of one model does not silently become another model's range.

## Brightness routes

These four routes have independent operation keys. Complete brightness operators accept relative linear RGB in an explicit basis M. Color conversion and extracted features use Float64. Each named feature and mapped-feature stage is published to Float64 before the next mathematical stage; final RGB is converted to T.

| Route | Extracted feature | Replacement |
| --- | --- | --- |
| ratio | Y from M*RGB | Scale original RGB by target/Y; at zero Y and nonzero target use neutral RGB at target; equal target copies |
| xyz_y | Y from M*RGB | Preserve X,Z, replace Y, then use inverse(M) |
| cielab_l | Convert M*RGB to CIELAB using the working reference white | Preserve a*,b*, replace normalized l, then invert Lab and M |
| oklab_l | Adapt working-white XYZ to D65 with Bradford, then convert to OKLab | Preserve a,b, replace L, invert OKLab and the Bradford adaptation, then use inverse(M) |

Whites use the FMT fixed D65 coordinates where required. Bradford is full adaptation; no adaptation occurs when the whites match. The XYZ-only-Y path may change chromaticity, and the perceptual paths preserve only their respective opponent coordinates before reconstruction. An unchanged published target coordinate bypasses the complete round trip and copies original RGB. Neutral monochrome uses the same routes with zero opponent coordinates, or RGB=(Y,Y,Y) for linear Y.

## Tile and ROI behavior

All members expose tile results, including statistical vectors and maps. Mathematical support remains E (matching elements), H(r) (finite neighborhood), W (a complete stated statistical domain), or S (ordered predecessors). Tile execution does not imply elementwise independence. The same output coordinates and fixed parameters produce the same result for whole-image, ROI and tile requests within native floating acceptance; discrete scan state must be independent of partition.

Spatial neighborhoods crop against the full declared image extent. CLAHE cells are a fixed algorithm grid; output tiles do not define the grid. Quantiles/histograms use an explicit metering ROI independent of the output ROI. Floyd-Steinberg uses the canonical full-image scan prefix for every requested pixel. Full-image support is permitted where the existing interface needs a finite static superset. No kernel changes are specified.

Empty requests have no dynamic payload dependencies. Bypass-only requests read only bypass inputs. Full auxiliary tables may be declared as support supersets, without additional numeric validation scans. Multi-output statistics define independent dependencies for counts, weights and feature values. Missing/unrequested values do not imply zeros.

## Scope and acceptance

These definitions cover single images and explicit control tensors. Time smoothing, automatic color-space inference, camera-history inference, file parsing, physical printing and commercial application matching are outside their mathematical scope. Kelvin/tint is a named illuminant model; palette dither consumes an explicit palette; halftone composition is an artistic color model.

Each member lists analytic fixtures. Also compare nonzero ROI and cross-tile positions, source/output dtypes, bypass components and any explicitly defined identity. The strict oracle validates its own references separately from native implementation comparison. Numerical reference results do not establish registration or runtime conformance.
