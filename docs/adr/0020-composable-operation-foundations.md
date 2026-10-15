# ADR 0020: Describe Typed Values and Infer Operation Outputs

- Status: Accepted
- Reader mirror: [Chinese](zh/0020-composable-operation-foundations.zh.md)
- Current boundary: the semantic facets, typed semantic descriptors, and output semantic rules of the original decision are superseded. Results carry structure only, and domain interpretation is an explicit static parameter of each operation. See [Data Model](../kernel-architecture/Data-Model.md), [Structured Results and tensor slots](../kernel-architecture/Global-Results.md), and [Tensor descriptions](../kernel-architecture/Tensor-Semantic-Metadata.md).

## 1. Core summary (TL;DR)

Composed workflows need every operation output to be known before execution. The compiler and operation registry share declarative input constraints, Result schema inference, explicitly bound validators and named output contracts. Callbacks run only after static metadata and parameters resolve successfully.

## 2. Mental model and intuition

`ValueDescriptor` describes how many samples exist and their physical scalar type. A Result schema groups typed tensor slots and packed fields; atomic trailing axes and spatial layout describe structural completeness and storage. Interpretation such as channel roles, color model or sampling grid is a static parameter of the operation that reads it, and an output does not inherit it from an input. The compiler resolves schemas before execution, while callbacks compute samples under the resolved contract.

```text
ordered inputs + static parameters
               |
       registry specialization
               |
  validate constraints and infer
     Result schemas (dtype / shape / slots)
               |
      SemanticGraphIR outputs
               |
        requested results
               |
   one PlanStep per demanded result
               |
 callback -> validate output -> publish
```

The C++ registry owns copied traits and immutable preparation. A C plugin supplies descriptor-owned records; the host validates and copies them before registry publication and holds a module lease while callbacks can run. Runtime Result and `Value` storage remains immutable after publication. A Result schema states structure, not that sample values satisfy a domain contract; explicitly bound validators check samples where an operation requires it.

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

struct OperationPortConstraint {
  std::vector<ExtensionIdentity> validators;
  OperationPortKind kind = OperationPortKind::Result;
  float minimum = 0.0F;
  float maximum = 0.0F;
  std::uint32_t element_type = 0;
  std::uint32_t rank = 0;
  std::uint32_t element_type_mask = 0;
  std::string result_schema_id;
  std::uint32_t result_schema_version = 0;
  std::string tensor_key;
  bool scalar_bounds = false;
};

struct OperationOutputTraits {
  std::string key = "value";
  std::optional<std::vector<std::uint32_t>> input_indices;
  OperationRegionRule region_rule = OperationRegionRule::Whole;
  OperationPortConstraint output_schema;
  std::optional<SchemaTemplate> result_schema;
};

struct OperationTraits {
  bool deterministic = true;
  bool side_effect_free = true;
  bool supports_cpu = true;
  bool supports_gpu = false;
  bool allows_cpu_fallback = false;
  std::uint32_t version = 26U;
  bool cacheable = true;
  std::vector<OperationParameterSpec> parameter_schema;
  std::vector<OperationPortConstraint> input_schema;
  std::vector<OperationOutputTraits> outputs;
};
```

These are contract excerpts, not complete construction code; other members are omitted. Operation plugins use Result C operation ABI 4. An operation declares up to 64 named outputs; output names are unique and map to declaration-order indices.

`ValueDescriptor` has rank 1 through 8 and nonzero extents. Its element type is independent of interpretation and memory layout. A `Value` can be strided; shape inference describes logical samples and does not imply dense contiguous storage. Sample bit validity belongs to the operation or its bound validators rather than the generic Value container. Input constraints can specify an exact dtype or an allowed dtype mask, rank, a fixed Result schema id and version, a named tensor member, explicit validators, or a finite Float32 scalar interval. Registry validation rejects contradictory or unknown constraint fields before publication.

A Result schema id and version name an explicit structural contract and enter compile identity; the kernel does not dispatch on them to a domain interpretation. A tensor slot declares its cell descriptor, batch prefix, `atomic_trailing_axes`, and optional spatial planar layout. Two Results with the same structure connect to the same port even when the caller assigns them different meanings. An operation that needs channel roles, color model, transfer, white point, alpha association, sampling or LUT axes reads them from an explicit static parameter, typically a `TensorDescription` encoded by the operations SDK (`Photospider::ops_sdk`). The operation validates that parameter against its input descriptor during preparation and does not copy it onto its output.

A port constraint binds validators by registered `ExtensionIdentity`; their order is significant and enters identity. The prepared operation runs exactly those validators on host-granted data. The kernel does not select a validator from a schema id, shape or channel count. The operations SDK owns the existing domain data contracts: the LUT3D bake report and owned-table Result schemas (`lut3d_bake_schema` and `lut3d_bake_table_schema` in `photospider/ops/numeric/lut3d_bake.hpp`) and the `QualityReport` type in `photospider/ops/numeric/quality.hpp`. `NumericDiagnostics` in `photospider/ops/numeric/diagnostics.hpp` is a payload codec for the generic diagnostic report channel, registered under `numeric_diagnostic_identity()` by `register_numeric_diagnostics`. The Layer types in `photospider/ops/composite/layer.hpp` are pure value types with validation and arithmetic functions and define no Result schema.

Each operation output resolves to a complete Result schema. A fixed output carries a registered `result_schema` prototype whose extents can be fixed, derived from an input axis or element count, tied to field rows, or counted at publication for a field. An operation whose traits set `requires_metadata_specialization` resolves its outputs per node through a pure specializer or `prepare_static` callback that reads only static input metadata and parameters. Static preparation also names the `ResourceIdentity` values its program needs. The compiler resolves schemas before any value callback and validates published results against them. Runtime samples do not choose dtype, rank or tensor shape; only a field declared with a runtime row count receives that count at publication.

An operation output has its own ordered input projection. The compiler infers metadata using the full declared signature, then lowers only the selected output's relevant inputs into its executable ancestry. A repeated-input template has a fixed prefix and one homogeneous suffix template; the resolved call expands to an ordered table and remains within the 1024-input bound. Resolved Result schemas, static parameters and repeated bounds are part of semantic identity.

A port with `scalar_bounds` accepts a complete Float32 tensor member with full sample shape `{1}` and no batch axes; another shape returns `TypeMismatch`. Its finite inclusive `[minimum, maximum]` interval is checked before every consumer callback, including when the value came from a result cache. An out-of-range computed value returns `OperationFailed`.

`numeric.sample_expression` accepts generic Float64 coefficients `[K]`, where `1 <= K <= 256`, finite `start`, positive finite `step` and an inferred Float32 output count from 1 to 1,048,576. The host parser limits source text to 4096 UTF-8 bytes, the expression tree to 256 nodes and depth 32. It accepts decimal/scientific literals, `x`, `c[index]`, parentheses, unary `+`/`-`, binary `+`, `-`, `*`, `/`, `^`, and the pure functions `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `min`, `max`. `lut.apply_1d` accepts a Float32 query and a rank-one Float32 table with at least two entries, and its output has the query's shape. The table sampling grid comes from the static parameters `sample_origin` and `sample_step`, and out-of-domain handling comes from `out_of_domain`, which is `reject` or `clip`. Both operations use Whole Region semantics.

Numerical precision and conversion behavior belong to each operation profile. The generic Value container preserves valid scalar bits, including floating signed zero and non-finite patterns where its physical type permits them. Operations add their own finite/range requirements, integer rounding, overflow behavior and reduction order. No graph-wide tolerance or bit-equivalence promise follows from a single operation's oracle.

## 4. Non-goals and explicit boundaries

- Result structure does not carry or prove image, mask, field, signal or LUT meaning.
- Operation outputs do not inherit an input's domain description. The kernel does not attach, propagate, match or automatically recognize domain labels.
- Static inference does not inspect runtime samples to choose dtype, rank or tensor shape.
- The compiler does not insert implicit casts, broadcasts, gamma operations, epsilon values, chromatic adaptation or gamut clamps.
- A `Region` rule is a data dependency contract. It does not specify worker partitioning or internal computation tiles.
- This decision does not add a script runtime, arbitrary expression language, filesystem access from operations, dynamic per-run output lengths, or implicit mutable state.
- This document defines reusable contracts, not a promise that every described operation family is registered in every build or supported by every backend.

## 5. Consequences

The compiler rejects malformed schemas, invalid static parameters, missing requested resources and unrepresentable checked extents before callbacks execute. Runtime output that differs from the inferred Result schema fails publication. Callers should build description parameters with the operations SDK codecs so static parameters match the canonical representation.

A tensor slot with an atomic channel axis receives complete logical channel coverage per pixel even when spatial Regions are partial. An H/W tile of RGBA retains all four channel samples for each pixel; planar storage may keep channels in separate planes, so logical channel completeness does not imply interleaved physical bytes. Slots without atomic trailing axes follow their sample coordinates exactly.

Resolved Result schemas and static inference records consume metadata memory and enter compiler/result identities. Changing a dtype, output schema, static description parameter or relevant-input projection changes those identities. Callbacks still own computational cost and output/scratch allocations; they must check cancellation in long loops and stay within declared resource bounds. Cache eligibility also requires deterministic, side-effect-free behavior and proven dependencies.
