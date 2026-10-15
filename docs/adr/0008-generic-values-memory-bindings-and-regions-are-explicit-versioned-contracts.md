# ADR 0008: Values and Input Bindings Carry Explicit Layout Contracts

- Status: Accepted
- Current boundary: the semantic facets, Value resource bindings, and alternative input binding forms of the original decision are superseded by the structural Result boundary. The sections below state the current contract; see [Data Model](../kernel-architecture/Data-Model.md), [Structured Results and tensor slots](../kernel-architecture/Global-Results.md), and [Tensor descriptions](../kernel-architecture/Tensor-Semantic-Metadata.md).

## 1. Core Summary (TL;DR)
A `Value` describes logical type, shape, valid coverage, and byte layout explicitly, and retains only its storage owner. Workflow declarations define required inputs with a fixed Result schema; each run supplies exactly one matching Result binding per declaration. This prevents pointers or payload bytes from silently defining graph meaning.

## 2. Mental Model & Intuition

```text
WorkflowInputDeclaration (id, name, result_schema)
                    |
                    +-- Run binding: ResultRef (Value or PlanarImage backing)
                    |
                    +-- schema check + authorized tensor Need --> operation
```

For an RGBA tensor shaped `[height, width, 4]`, the channel axis can remain whole while a planner requests a rectangular spatial region. Logical region coordinates describe pixels/elements; strides describe addresses. Planar images retain separate channel planes rather than claiming interleaved storage.

## 3. Formal Contracts & APIs

```cpp
struct ValueDescriptor { ElementType element_type; std::vector<std::uint64_t> shape; };
struct StridedLayout { std::uint64_t byte_offset; std::vector<std::int64_t> byte_strides; std::vector<std::uint64_t> origin; };
class Value {
 public:
  static Result<Value> create(ValueDescriptor descriptor, Region region,
                              StridedLayout layout,
                              std::vector<std::uint8_t> bytes);
};
struct WorkflowInputDeclaration { std::uint64_t id; std::string name; std::shared_ptr<const SchemaTemplate> result_schema; };
struct ExecutionBinding { std::string name; ResultRef result; };
```

The element vocabulary currently includes `UInt8`, `Int8`, `UInt16`, `Int16`, `Int64`, `Float32`, and `Float64`. Shape rank is 1..8 with nonzero extents. A `Region` is rank-matching logical coverage using half-open intervals. A layout uses signed byte strides and an origin; validation checks the complete addressed range against retained storage, including negative and zero strides. Value bytes use immutable shared storage.

A Value holds its descriptor, Region, layout, and shared immutable storage owner. Color, sampling, and LUT interpretation are explicit static parameters of the consuming operation. Immutable operation resources, such as profiles, reach operation preparation through the `ResourceSet` supplied to the compiler, and prepared operations, plans, and execution state retain them.

Empty Region coverage is valid for a generic Value, although operation ports may require nonempty coverage. See [Region semantics](../kernel-architecture/Region-Semantics.md).

Each declared input is bound exactly once on each Run, including unused declarations. Every declaration carries a complete `result_schema`; a missing schema fails compilation with `InvalidArgument`. The bound `ResultRef` must match that structural schema before callbacks run. Caller containers stay unchanged while copied; Result storage keeps shared immutable ownership. Payload bytes are run data, not compiler identity.

## 4. Non-Goals & Explicit Boundaries
- A logical region is not a byte range and does not imply physical contiguity.
- Generic Values do not imply persistence or a serialization format.
- Values and Results carry no semantic facets or attached domain descriptions, and they do not retain operation resources.
- `Value` and `PlanarImage` provide backing storage; they are not alternate workflow input forms.
- Planar storage is not interleaved RGBA storage. Writable external producer binding is not inferred from a pointer.

## 5. Consequences
Malformed rank, shape, bounds, or layout fail before Value publication. Binding schema differences fail validation before callbacks. Bad bounds, overflow, and bounded payload exhaustion can return typed failures or allocation exceptions according to the failing operation. Callers must supply operation resources through the compiler's `ResourceSet` and keep borrowed callback buffers only for their documented lifetime.
