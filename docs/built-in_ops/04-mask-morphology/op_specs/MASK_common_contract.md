---
spec_schema_version: 1
id: MASK-common
kind: shared_operator_contract
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
---


# MASK: shared specification

This specification defines the MASK-01..17 semantics through explicit members and family contracts. There are 63 members. MASK-16B/C are public composite authoring workflows; their reusable `mask.prepare_gap_fill_strict` stage is an additional supporting registry key, not an extra member. The strict profile is the only numerical profile defined here.

## Authority and inherited numerical rules

Inherit [NUM-common](../../01-numeric/op_specs/NUM_common_contract.md), [NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md), [FMT-common](../../02-format-color/op_specs/FMT_common_contract.md), [FMT-COLOR](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md), [relative coordinates](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) and the [planar/codec boundary](../../02-format-color/op_specs/FMT_codec_boundary.md).

Precedence: member > family > this file > inherited generic baseline. FMT's explicit interpretation policy and NUM numerical accuracy both apply. References import only the named shared obligations, not another operation's ports, defaults or complete-input callback.

## Static structure and unchecked premises

Compilation checks schema/version, member directories, dtype, rank, positive extents, axes and shape relations, parameter presence, types and syntax. Runtime counts, offsets, indices, label IDs, associations, canonical padding/encoding and numerical/geometric conditions are unchecked caller premises. Finite/range/order/positivity conditions remain numerical premises even when supplied as static parameters. Producers construct the specified results and consumers trust those premises, without payload scans.

Outside those premises no result, diagnostic or recovery is guaranteed. Algorithmic predicates, candidate selection, explicit threshold/clipping behavior, rounding refinement and requested quality measurements are preserved. Defined no-solution outcomes for valid inputs are part of the operator behavior.

## Numerical authority

All numerical rounding and strict/accelerated accuracy rules are inherited from [NUM-common](../../01-numeric/op_specs/NUM_common_contract.md) and [NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md). MASK defines no alternative rounding mode or tolerance.

RN_T denotes the NUM nearest/ties-to-even result in the declared Float32/Float64 dtype, with gradual underflow. Strict expressions retain the rounding boundaries stated by each member; Float64 is not narrowed. Copies, discrete predicates, indices, counts, selected endpoints and special values follow the exact NUM obligations. MASK members specify their formulas, domain restrictions, signed-zero selection rules and stage boundaries.

Stored floating samples and static parameters represent their actual binary values. Numerically consumed samples exclude NaN as an unchecked premise. The distance no-feature infinity convention below defines a domain value, not permission to suppress finite overflow. Integer dimensions, coordinates, IDs and counts use exact arithmetic. Generated Binary values are floating +0 and 1; the Binary input domain includes either sign of zero. Identity copies retain bits.

## Tensor domains and ports

All ports carry Results with fixed Tensor member schemas. A member's tensor notation describes its Result payload, and all listed inputs are required unless explicitly optional. Member port order is normative.

Spatial primitives use one rank-two `[H,W]` tensor with positive H,W and N=H*W at most 2^40. Empty images are not defined; an empty requested Region is legal. Per-image batching is explicit authoring, not an implicit extra axis. MASK-04/11/17 use planar `[C,H,W]` as expressly listed, with C positive and total elements <=2^40.

Coordinates are global, zero-based `(y,x)`, pixel centers at those integer coordinates, cells `[y-.5,y+.5] x [x-.5,x+.5]`. x grows right; y grows down. Spacing `(sy,sx)` consists of positive finite Float64 values in an explicitly selected distance unit; default authoring units are pixels with both spacings 1. No metadata-derived mm/px conversion occurs implicitly.

| Alias | Storage and consumed sample domain | Output meaning |
| --- | --- | --- |
| Coverage | Float32 or Float64, finite [0,1] | Soft mask; not a color/alpha attachment |
| Binary | Float32/Float64, exactly numeric 0 or 1 | Binary membership; 255 is outside the domain |
| Field | Float32/Float64, finite, signed/HDR allowed | Numeric field, not coverage |
| Distance | Float32/Float64, nonnegative or signed; no-feature infinities as specified | Explicit units/metric/basis/no-feature semantics |
| Labels | Int64, 0 background, positive foreground IDs | Explicit labeling basis, not a color plane |

Floating inputs to one arithmetic member have the same dtype unless the member explicitly allows independent types. Coverage/binary-preserving outputs retain input dtype. MASK-03 takes Field; MASK-04 takes Field color components; MASK-10 uses an explicit output_dtype. No Float16, UInt8 mask storage, packed bits, implicit UInt8 255 normalization, automatic thresholding or hidden conversion is defined. MASK-03E accepts raw UInt8 bytes only as an explicit input conversion and emits Float32/Float64 Binary selected by output_dtype. Other mask outputs preserve the primary mask input dtype; flood outputs use the seeds dtype, MASK-12G uses an explicit output_dtype, and validity/within flags use their associated value dtype. Skeleton outputs preserve the input Binary dtype independently of radius dtype. Binary-only consumers assume the binary domain on their required sample set. Floating coverage morphology computes directly on soft samples.

Results carry structural schemas and samples. Spatial coordinates, coverage/binary meaning, distance units/metric and label basis are explicit operation configuration or declared schema semantics. Outputs carry no attached descriptions or sample-validity proofs; the caller supplies the next consumer's interpretation. Semantic color selection uses a complete static group description in the operations-SDK TensorDescription v4/v5 codec and native CIELAB/CIELCh l=L*/100. Only the formula's selected coordinates are numerical inputs. Raw channel arithmetic reads only its formula operands. MASK-04E/F use the registered operations-SDK model/skipped schema factories described in their specs. MASK-12D/E/F use the complete area-index, bbox-index and component-bundle factories in the same operations SDK.

## Static parameters

All statics listed by a member are supplied by direct WorkflowDocument nodes; authoring recommendations are serialized by helpers. Unknown, missing, ill-typed and conditionally irrelevant fields are rejected before payload reads (InvalidArgument / InvalidDomain for missing/mistyped parameters, invalid syntax or enum, or a static structural constraint; TypeMismatch for wrong dtype/rank/shape/basis or unsupported semantic description). Finite Float64 statics do not accept a Boolean as a number. Enum tokens are case-sensitive. Integer parameters are true Int64, never routed through double. Selectors use NUM's canonical comma-separated nonnegative index String syntax. An ordered unique channel list preserves order and has exactly the declared C entries. The custom footprint encoding is described in MASK-05. Output shapes and static Result schemas are inferred solely from descriptors and statics. Effective K is an ordinary Int64[1] payload; unused rows of a fixed positive capacity are zero-filled.

Every registered member also accepts an optional static Int64 `max_work` (nonnegative, default INT64_MAX), an operational work guard that does not alter results. MASK-16B/C forward it separately to each authored primitive.

## Boundary and support

`Q` is the mathematical output set and `D` is the finite image canvas. `Data` and `Control` describe mathematical sample support. Missing coverage is not zero. Out-of-canvas zero is a mathematical constant of the member where stated, not a stored sample.

Elementwise members depend on exactly the same positions. A finite footprint B uses support `{p+b : p in Q, b in B} intersect D`, or the explicitly defined boundary mapping. A composed operator's support propagates through all stages, not only the first stage's halo. A complete-input member requires the complete specified input domains for any nonempty Q; it remains a global operation under tiling and no fixed-halo shortcut applies.

MASK-05 and MASK-06 use zero extension on the infinite integer lattice. For opening/closing the intermediate is not cropped and re-zeroed: the mathematical stages are evaluated outside D where needed and only the final output is cropped. Composite workflows therefore use explicit pad/stage/crop. This distinction is observable on a full 1x1 image. Other boundary choices are not inherited from SciPy/OpenCV defaults. Gaussian alone adds named replicate and half-sample-reflect boundary mappings.

Results for any Region equal the corresponding region of the whole-canvas result. Member default `layout=materialize` is an authoring policy, not an enum accepted by every primitive. Selected outputs of one call are produced together. Static capacities, ordinary count payloads and source ObjectId associations are constructed according to the member contract. Missing coverage is not an all-zero table.

## Distance infinities and defined statuses

Distance is the only domain here admitting no-feature infinities. Full unsigned nearest distance returns +Inf when no site exists; signed center distance returns +Inf for background without foreground and -Inf for foreground without background. Missing nearest coordinates are (-1,-1); coordinate sentinels alone do not establish absence. Finite-distance arithmetic must remain representable as a caller premise; an overflowing finite computation is outside that domain and is not a no-feature result. NaN is outside the Distance domain. Truncated distance returns the signed/unsigned finite limit and a Binary within flag. Field and Coverage remain finite-only.

Distance-aware consumers MASK-07B/C and MASK-09 admit these infinities: finite shifts preserve the sign of infinity; threshold at finite r maps -Inf to 1 and +Inf to 0; feather maps -Inf to 1 and +Inf to 0, including zero-width feather.

An algorithmic refinement exhausted on valid inputs, or a declared output capacity exceeded on valid inputs, is a member-defined operational failure. MASK-17C `empty_policy=error` with a zero denominator is OperationFailed; selecting valid alone evaluates no image samples and returns zero validity without that value failure.

## Acceptance

Every member gives an analytic case. Numeric checks do not replace schema checks. Local members produce results equal across whole-canvas and selected-Region evaluation, including nonzero-origin and disjoint Regions, odd strides and empty Q. Global members use the whole demand, with cross-tile connectivity and no fixed-halo shortcut. Valid special values (signed zeros, dtype extremes, no-feature and empty-component results) and correctly paired associations are exercised. External sources establish definitions or feature existence only; commercial bit identity is not claimed.
