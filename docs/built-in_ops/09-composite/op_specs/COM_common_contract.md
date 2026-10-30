---
spec_schema_version: 1
id: COM-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# COM: shared compositing and keying contract

This contract covers basic CMP coverage, BLN color functions, NBL color-property functions and KEY matte/evidence operations. Layer trees, groups and mask attachment are authored by connecting these basic operators. Painting and repair members belong to a separate specification scope.

## Ports and structural parameters

Each ordered port is a Result with one tensor at member index zero. Input member keys are unconstrained. Each output Result uses schema `photospider.tensor`, version 1, and tensor key `samples`; output names are given in member signatures. `T` is Float32 or Float64; all T operands in a call have the same dtype, and T outputs retain it. Named scalar inputs have shape [1]. A named port is required; a suggested value describes what the caller may bind, not an optional argument.

`S` is the ordered non-component shape. `[S,C]` denotes a logical image with a component axis; `axis:Int64` and ordered `components:Array[Int64]` select its color slots, with `alpha_index:Int64` where alpha is required. These selectors may instead be resolved from an explicit static `input_description` under the existing FMT rules. Static descriptions provide model, basis, white, transfer and reference units; none are attached to or propagated by Results. Matching interpretations are caller premises. Descriptor/type/shape and selector consistency checks follow the [foundation](../../00-foundation/contracts.md).

Color-only `[S,K]` ports contain exactly K=1 Gray or K=3 RGB coordinates; native Lab/OKLab ports contain exactly three model coordinates. They have no implicit alpha. `[H,W]` functions use y,x order, positive extents and a full-image integer origin. Explicit frame stacks have `[N,H,W,3]` with their named frame axis. Stored rank is at most 8 and logical element count at most 2^40. No implicit broadcasting, resize, alpha insertion or frame alignment is performed.

Same-shape image functions select the same color/alpha slots in matching inputs. Ordinary CMP outputs copy unrelated background AOVs; BLN-20 copies unrelated slots from its first image. Selectors and clear have their explicitly different copy/zero rules. Copies preserve bits, including signed zero and NaN payloads. Ordinary straight image alpha is independent of color; a four-channel shape alone does not identify RGBA. Missing alpha is explicitly added upstream where required.

Static parameters outside signatures have the displayed types and defaults. Unspecified defaults mean required arguments. Image, matte and control domains are unchecked caller premises. Alpha/coverage/soft mask is in [0,1], binary masks are UInt8 0/1, and trimap labels are UInt8 0/1/2. Invalid numerical payloads have no promised runtime diagnostic. Structural checks and operational failures retain the foundation behavior.

## Numerical target and strict reference

Production operators evaluate the declared equations with native FP32/FP64 arithmetic. Scalar point formulas use T; reductions, matrix construction and linear-system solving may use Float64, and explicitly declared Float64 values retain their dtype. Fused arithmetic and equivalent reassociation are allowed within the continuous comparison tolerances and cannot change output as a function of tile partition. Discrete selection, ties, connectivity and coordinate-dependent randomness retain their specified rules.

Consumed active colors/controls are finite and satisfy the member domain. Source copies remain bit-preserving. Native floating overflow yields signed infinity, invalid arithmetic yields NaN, and underflow follows ordinary IEEE behavior; a subsequent color operation may require finite input. No automatic global gamut or [0,1] clamp is inserted. An explicit bounded formula or generated coverage retains its declared clamp. A reject domain is an unchecked premise rather than a required scan.

The [independent oracle](../../../../oracle/ops/09-composite/README.md) treats stored inputs exactly, evaluates rational formulas and certified radicals, and rounds to the declared output dtype using nearest/ties-to-even. Named stored-distance/measure stages also round. Small matting systems use exact rational solutions before final clamp and rounding. The oracle is the comparison target, not a requirement for production arbitrary-precision calculation.

For the analytic fixtures, continuous finite values use abs(error)<=atol+rtol*abs(reference), with (atol,rtol)=(2e-6,2e-5) for Float32 and (2e-12,2e-11) for Float64 in the native coordinate units. Ill-conditioned solve fixtures require explicit output-error inspection in addition to the residual criterion; a residual alone is not a general forward-error bound. Copies, mask endpoints, labels, ties and selected tuples compare exactly. A discrete decision mismatch is reported separately and is not excused by small color error. These fixture tolerances do not assert a universal bound for every extreme input.

## Working domains and alpha

CMP arithmetic has distinct linear and encoded members. Linear members operate on the declared linear RGB/Gray coordinates. Encoded members apply the same coverage equations directly to the declared encoded coordinates. Inputs, supplied blended color and output all share that member's working domain. Domain conversion is an explicit FMT operation and has its own output boundary.

BLN color functions accept the explicitly selected linear or encoded artistic space where the member permits it. NBL receives native CIELAB, OKLab or bounded W3C RGB tuples. Convert those tuples explicitly before a CMP member that expects another space. Native CIELAB lightness is l=L*/100; CIE a*,b* retain their conventional units, while OKLab uses native L,a,b. Model constants and interpretation inherit the [FMT mathematics](../../02-format-color/op_specs/FMT-11_model_math.md); COM retains its own native arithmetic policy.

Layer opacity and masks are explicit data transformations: compute effective source alpha from its original alpha, a coverage mask and opacity, then supply the modified straight image to CMP. Color blend amount is a separate interpolation of color-function outputs. Each composed node retains its declared numerical boundary. Raw color mass, coverage alpha, averaging weight and independently visible Layer emission have separate meanings; these operators do not redefine the existing Layer SDK contract.

Generated composite color is +0 when the computed output alpha is zero. Explicit source/destination selection and image-mix endpoints retain their source hidden colors. Atop copies its retained alpha bits. Underflow and intermediate staging follow native arithmetic, so an implementation must compare tiny-alpha cases to the strict reference and report any differences; hidden color is not an emission channel.

## Tile, support and result behavior

Every member is a tile operator. E denotes matching-element support, H(r) finite support, W a stated global domain, and frame support means the same coordinates across the explicit frame stack. Global matting, border-connected hole filling and nearest-background estimation retain their full mathematical dependence. ROI and tile boundaries are not image edges or new solve boundaries. The same requested coordinates use the same global data, parameters and deterministic choices.

Empty output requests read no payload. A copy-only or independent output reads only its named source/control dependencies; no extra validation plane is added. Mathematical coefficient-zero branches follow their declared relations. Full-image/table support may be used as a finite static superset where necessary. A confidence output uses evidence flags and does not require the corresponding color solution.

Insufficient evidence or a singular matting system yields existing `OperationFailed` with a bounded `NoSolution` diagnostic when an affected color/matte output is requested. Failure to meet an explicitly requested solve criterion yields `OperationFailed` with `NotConverged`. These are diagnostic descriptions, not new ABI enums. Upstream failures, cancellation and resource failures retain their existing categories. Returned successful observations obey the foundation ownership/publication contract.

## Scope and verification

The members provide mathematical primitives. Higher-level layer/group/conditional-mask policies, UI capture and painting are outside this contract. Single-frame inputs and explicitly supplied aligned frame stacks are distinguished. No implicit tracking, background history, clock-based randomness or commercial application matching is introduced.

Each member supplies analytic cases. Compare strict-reference values, zero/nonzero ROI, cross-tile coordinates, independent color/alpha/confidence requests, both image dtypes and explicit identity/copy branches. The numerical oracle's self-checks do not establish runtime registration or integration conformance.
