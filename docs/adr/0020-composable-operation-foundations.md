# ADR 0020: Describe Typed Values and Infer Operation Outputs

- Status: Accepted
- Reader mirror: [Chinese](zh/0020-composable-operation-foundations.zh.md)

## 1. Core summary (TL;DR)

Composed workflows need operations to preserve, transform or drop semantic meaning as well as compute bytes. The compiler and operation registry share declarative input constraints, output shape and dtype inference, semantic rules and named output contracts. Callbacks run only after static metadata and parameters resolve successfully.

## 2. Mental model and intuition

`ValueDescriptor` describes how many samples exist and their physical scalar type. A `ValueFacet` adds an interpretation such as image channels and color model. The compiler propagates these descriptions before execution, while callbacks compute samples under the resolved contract.

```text
ordered inputs + static parameters
               |
       registry specialization
               |
  validate constraints and infer
      dtype / shape / facets
               |
      SemanticGraphIR outputs
               |
        requested results
               |
   one PlanStep per demanded result
               |
 callback -> validate output -> publish
```

The C++ registry owns copied traits and immutable preparation. A C plugin supplies descriptor-owned records; the host validates and copies them before registry publication and holds a module lease while callbacks can run. Runtime `Value` storage remains immutable after publication. The semantic descriptor is metadata, not a claim that the samples satisfy it; operation validation checks samples where the contract requires.

## 3. Formal contracts and APIs

```cpp
enum class ElementType : std::uint32_t {
  UInt8 = 1, Int64 = 2, Float64 = 3, Float32 = 4,
  Int8 = 5, UInt16 = 6, Int16 = 7
};

struct ValueDescriptor {
  ElementType element_type;
  std::vector<std::uint64_t> shape;  // rank 1..8; every extent is positive
};

struct ValueFacet {
  std::string key;
  std::uint32_t version;
  std::vector<std::uint8_t> payload;
};

struct OperationOutputTraits {
  std::string key = "value";
  OperationShapeRule shape_rule = OperationShapeRule::Scalar;
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  OperationDtypeRule output_dtype_rule = OperationDtypeRule::Declared;
  OperationSemanticRule output_semantic_rule = OperationSemanticRule::Drop;
  std::vector<OperationExtent> output_axes;
  std::optional<std::vector<std::uint32_t>> input_indices;
};

struct OperationTraits {
  bool deterministic = true;
  bool side_effect_free = true;
  bool supports_cpu = true;
  bool supports_gpu = false;
  bool allows_cpu_fallback = false;
  bool cacheable = true;
  std::vector<OperationPortConstraint> input_schema;
  std::vector<OperationOutputTraits> outputs;
};
```

These are contract excerpts, not complete construction code. The current C++ `OperationTraits` record is version 22. The package is version 0.30.0, and operation plugins use Result C ABI 2; the former Base C and planar operation tables are removed. A plugin descriptor declares up to 64 named outputs; output names are unique and map to declaration-order indices. A single-output operation explicitly uses `value`.

`ValueDescriptor` has rank 1 through 8 and nonzero extents. Its element type is independent of semantic interpretation and memory layout. A `Value` can be strided; shape inference describes logical samples and does not imply dense contiguous storage. Sample bit validity belongs to the operation or typed semantic contract rather than the generic Value container. Input constraints can specify an exact dtype or an allowed dtype mask, rank, semantic kind/facets, or a finite scalar interval. Registry validation rejects contradictory or unknown constraint fields before publication.

`SemanticDescriptor` is the owned C++ representation encoded by the public `encode_semantic` helper. Image descriptors use facet `photospider.image` version 2; the other typed semantic kinds use `photospider.semantic` version 1. A generic Value can omit semantic facets. Decoding accepts the current canonical format and does not translate an older image facet. Canonical metadata is limited to 4096 encoded bytes, each text field to 128 UTF-8 bytes and channel count to 64. Unused fields are empty or positive zero; metadata numbers must be finite. The `semantic_parameter` helper returns lowercase hexadecimal capped at 8192 characters.

For an `Image` `SemanticDescriptor`, the logical Value shape is Float32 HWC. The first image profile is linear-sRGB/Rec.709 RGB with D65 white and relative scene-referred values; finite signed and HDR RGB values are valid. RGB, XYZ and Lab use explicit channel roles, units and reference white. Alpha, when present, is the final coverage channel with samples in [0,1]. Straight alpha may preserve hidden color at zero alpha; coverage-premultiplied RGB is zero at zero alpha. Unassociation divides by each positive alpha without an epsilon and returns zero RGB at zero alpha. It can lose hidden straight color when associating at zero alpha. Nonlinear conversions require unassociated color, use the declared white and do not apply implicit chromatic adaptation or gamut clipping. Logical HWC axes do not prescribe whether storage is interleaved or planar.

Other semantic kinds describe masks, scalar/vector/complex fields, sampled signals, LUTs, byte resources and image planes. A mask distinguishes coverage, probability and membership. A vector field declares coordinate space and direction. A complex field declares real/imaginary components and the unshifted frequency convention. An `ImagePlane` stores one Float32 HW plane with explicit nominal sampling origin and positive Y/X steps; that logical sampling description does not itself prove dependency support or physical storage layout.

Operation outputs declare dtype and shape independently. The registry supports scalar, preserve-first-input, match-all-inputs, fixed, shrink and statically described axes. Dtype rules select a declared type, an input type, or an allowed static parameter rule. An axis can resolve from a constant, static parameter, input axis/count, canonical channel-index list length or a bounded finite Float64 parameter rounded up. The axis rule can apply a checked subtraction, ceil division, positive multiplier and nonnegative constant offset. The compiler resolves descriptors before any value callback and validates published results against inferred dtype, shape and facets. Runtime samples do not choose output shape.

The output semantic rule explicitly preserves an input facet, establishes facets, reads a static semantic parameter, or drops typed guarantees. Shared rules also describe channel extraction/selection/merge, alpha association changes, and supported color-model conversions. A generic intermediate does not regain lost semantics from its shape; a later merge or metadata assignment must declare the target meaning and validate its constraints. Canonical channel-index parameters contain 1..64 decimal indices from 0 through 63, separated by commas, with no spaces, signs or leading zeroes. The helper APIs construct and parse these values; callers do not assemble semantic payload hex by hand.

An operation output has its own ordered input projection. The compiler infers metadata using the full declared signature, then lowers only the selected output's relevant inputs into its executable ancestry. A repeated-input template has a fixed prefix and one homogeneous suffix template; the resolved call expands to an ordered table and remains within the 1024-input bound. Dtype and shape inference, static parameters and repeated bounds are part of semantic identity.

The `Float32Scalar` port accepts a complete Float32 `{1}` with no facet, a dimensionless `Scalar` facet, or a dimensionless single-sample `SampledSignal` facet. Its finite inclusive range is checked before every consumer callback, including when the value came from a result cache. A direct binding with an invalid sample returns `InvalidArgument`; an invalid computed value returns `OperationFailed`; an incompatible facet returns `TypeMismatch`.

The public expression rule accepts generic Float64 coefficients `[K]`, where `1 <= K <= 256`, finite start, positive finite step and an inferred Float32 output count from 1 to 1,048,576. Its output is a dimensionless sampled signal with dimensionless sample-axis units. The host parser limits source text to 4096 UTF-8 bytes, the expression tree to 256 nodes and depth 32. It accepts decimal/scientific literals, `x`, `c[index]`, parentheses, unary `+`/`-`, binary `+`, `-`, `*`, `/`, `^`, and the pure functions `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `min`, `max`. `lut.apply_1d` accepts a Float32 sampled-signal query and a Float32 one-channel Signal/LUT table with at least two uniformly sampled entries. Query sample units must match the table axis unit; out-of-domain handling is explicit `reject` or `clip`. Both operations use Whole Region semantics.

Numerical precision and conversion behavior belong to each operation profile. The generic Value container preserves valid scalar bits, including floating signed zero and non-finite patterns where its physical type permits them. Operations add their own finite/range requirements, integer rounding, overflow behavior and reduction order. No graph-wide tolerance or bit-equivalence promise follows from a single operation's oracle.

## 4. Non-goals and explicit boundaries

- Shape coincidence alone never proves image, mask, field, signal or LUT meaning.
- Static inference does not inspect runtime samples to choose dtype, rank or shape.
- The compiler does not insert implicit casts, broadcasts, gamma operations, epsilon values, chromatic adaptation or gamut clamps.
- Generic arithmetic does not preserve typed guarantees unless its declared semantic rule proves the result meaning.
- A `Region` rule is a data dependency contract. It does not specify worker partitioning or internal computation tiles.
- This decision does not add a script runtime, arbitrary expression language, filesystem access from operations, dynamic per-run output lengths, or implicit mutable state.
- This document defines reusable contracts, not a promise that every described operation family is registered in every build or supported by every backend.

## 5. Consequences

The compiler rejects malformed schemas, unsupported semantic combinations, invalid static parameters and unrepresentable checked extents before callbacks execute. Runtime output that differs from the inferred descriptor or facet contract fails publication. Callers should use the semantic and channel-list helpers so static parameters match the canonical representation.

Typed images require complete logical channel coverage per pixel even when spatial Regions are partial. An H/W tile of RGBA retains all four channel samples for each pixel; planar storage may keep channels in separate planes, so logical channel completeness does not imply interleaved physical bytes. Generic and other typed arrays follow their own Region contract.

Semantic descriptors and static inference records consume metadata memory and enter compiler/result identities. Changing a channel role, dtype rule, output shape rule or relevant-input projection changes those identities. Callbacks still own computational cost and output/scratch allocations; they must check cancellation in long loops and stay within declared resource bounds. Cache eligibility also requires deterministic, side-effect-free behavior and proven dependencies.
