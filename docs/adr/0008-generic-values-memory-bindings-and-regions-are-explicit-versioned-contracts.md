# ADR 0008: Values and Input Bindings Carry Explicit Layout Contracts

- Status: Accepted

## 1. Core Summary (TL;DR)
A `Value` describes logical type, shape, valid coverage, byte layout, semantic facets, and retained resources explicitly. Workflow declarations define required inputs; each run supplies exactly one matching binding per declaration. This prevents pointers or payload bytes from silently defining graph meaning.

## 2. Mental Model & Intuition

```text
WorkflowInputDeclaration (type, shape, region, layout, facets)
                    |
                    +-- Run binding: Value | RegionalSource | InputSnapshot | PlanarImage
                    |
                    +-- validated read or exact demand --> operation
```

For an RGBA tensor shaped `[height, width, 4]`, the channel axis can remain whole while a planner requests a rectangular spatial region. Logical region coordinates describe pixels/elements; strides describe addresses. Planar images retain separate channel planes rather than claiming interleaved storage.

## 3. Formal Contracts & APIs

```cpp
struct ValueDescriptor { ElementType element_type; std::vector<std::uint64_t> shape; };
struct StridedLayout { std::uint64_t byte_offset; std::vector<std::int64_t> byte_strides; std::vector<std::uint64_t> origin; };
class Value {
 public:
  static Result<Value> create(ValueDescriptor, Region, StridedLayout,
      std::vector<std::uint8_t>, std::vector<ValueFacet> = {}, ResourceBindings = {});
};
struct WorkflowInputDeclaration { std::uint64_t id; std::string name; ValueDescriptor descriptor; Region region; StridedLayout layout; };
struct ExecutionBinding { std::string name; Value value; /* alternatively source, snapshot, or image */ };
```

The element vocabulary currently includes `UInt8`, `Int8`, `UInt16`, `Int16`, `Int64`, `Float32`, and `Float64`. Shape rank is 1..8 with nonzero extents. A `Region` is rank-matching logical coverage using half-open intervals. A layout uses signed byte strides and an origin; validation checks the complete addressed range against retained storage, including negative and zero strides. Value bytes use immutable shared storage.

A Value has at most 64 facets. Each key is unique printable ASCII of at most 256 bytes, and the host sorts facets by key before publication. Each facet payload is at most 64 KiB; validation also bounds the aggregate payload. Resource bindings retain explicit owners.

Empty Region coverage is valid for a generic Value, although operation ports may require nonempty coverage. See [Region semantics](../kernel-architecture/Region-Semantics.md).

Each declared input is bound exactly once on each Run, including unused declarations. The binding must match declared metadata and required whole coverage. Exactly one input source form is selected. Caller containers stay unchanged while copied; immutable source state must support concurrent reads. Payload bytes are run data, not compiler identity.

## 4. Non-Goals & Explicit Boundaries
- A logical region is not a byte range and does not imply physical contiguity.
- Generic Values do not imply persistence or a serialization format.
- Planar storage is not interleaved RGBA storage. Writable external producer binding is not inferred from a pointer.

## 5. Consequences
Malformed rank, shape, bounds, facets, or layout fail before Value publication. Binding shape/layout/facet differences fail validation before callbacks. Bad bounds, overflow, and bounded payload exhaustion can return typed failures or allocation exceptions according to the failing operation. Callers must retain explicit resource owners and keep borrowed callback buffers only for their documented lifetime.
