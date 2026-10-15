# Data Model

## 1. Scope and ownership

The compiler owns immutable workflow structure and inferred structural metadata. Structured operations exchange `ResultRef` owners through named bindings; a Result contains typed tensor slots, packed fields, or both. `Value` remains a typed backing and local data container, while `PlanarImage` provides image storage. Returned Result owners retain their storage and accounting leases after the execution context retires.

Result and Value carry structure, sample data, and the owners that their storage needs. They carry no attached domain description: color space, white point, transfer, alpha association, sampling, and LUT interpretation are explicit static parameters of each operation that needs them. Operation-owned immutable resources, such as ICC profiles, stay in the plan and execution state through `ResourceSet`, not in the Result.

## 2. Core structures and memory layout

```cpp
struct WorkflowDocument {
  std::uint32_t schema_version = 6;
  std::vector<WorkflowInputDeclaration> inputs;
  std::vector<WorkflowNode> nodes;
  std::vector<WorkflowOutput> outputs;
};

class Region;  // logical offset/extent intervals in descriptor-axis order

class Value {
 public:
  static Result<Value> create(ValueDescriptor, Region, StridedLayout,
                              std::vector<std::uint8_t>);
  // Checked byte offset of one logical coordinate inside bytes().
  Result<std::size_t> byte_address(const std::uint64_t* coordinate,
                                   std::size_t rank) const;
  Result<std::size_t> byte_address(
      std::initializer_list<std::uint64_t> coordinate) const;
  Result<std::size_t> byte_address(
      const std::vector<std::uint64_t>& coordinate) const;
};

struct ExecutionBinding {
  std::string name;
  ResultRef result;
};

class PlanarImage {
 public:
  static Result<PlanarImage> create(const ValueDescriptor&,
                                    const PlanarImageConfig&);
};
```

`WorkflowDocument` is copied compiler input. Its nodes carry operation keys, ordered input references, typed parameters, and named output selections. Input declarations require a fixed `result_schema`; compiler validation rejects a missing schema with `InvalidArgument`. Execution bindings provide the corresponding owning `ResultRef`, whose structural schema is checked against that declaration. The compiler rejects documents whose `schema_version` is not 6, including earlier label-based workflows. Plans keep declarations and selected resource identities, not caller sample addresses.

A generic `Value` has a nonzero rank-1-to-8 descriptor, a logical `Region`, a `StridedLayout`, and shared immutable CPU-accessible storage. Its element types are `UInt8`, `Int8`, `UInt16`, `Int16`, `Int64`, `Float32`, and `Float64`. `ValueDescriptor` holds only the element type and logical shape. `Value::create` validates shape and coverage, stride count and addressed byte span, and element type before publishing. Negative and zero strides are valid when every addressed byte remains in the backing allocation. Copies share storage and expose no writable pointer.

`Value::byte_address` maps one logical coordinate to a checked byte offset into `bytes()`. It applies the Region origin and the layout strides, including negative strides, broadcast (zero) strides and high origins. A coordinate outside coverage, a rank mismatch, a null coordinate pointer or a default-constructed Value returns `InvalidArgument`. The pointer-plus-rank and brace-list overloads borrow the coordinate only for the duration of the call and perform no allocation on the success path, so they suit per-sample loops; for example, `value.byte_address({y, x, c})` addresses channel `c` of pixel `(x, y)` in an HWC image without building a vector. The vector overload has the same result. `ValueView` forwards all three overloads. Concurrent calls on the same immutable Value are safe.

`Region` stores unsigned offset/extent pairs in descriptor-axis order. It describes logical samples, not bytes. Region interval and element-count arithmetic is checked. `Value::as_float64()` accepts only a contiguous Float64 scalar whose Region is exactly rank one with `{offset=0, extent=1}`; other valid Value coverage remains usable through the general accessors.

`BufferAllocator` reserves capacity before allocation. Move-only mutable buffers transfer their lease into immutable storage at publication. `Value::from_storage` and `view` can share backing without copying; `bytes()` returns a borrowed view and `copy_bytes()` makes an explicit caller-owned copy. Storage can outlive the allocator and execution context. Regional-source callbacks fill a requested packed Region synchronously; their writable destination and allocator pointers expire when the callback returns.

`PlanarImage` represents structural rank-two or rank-three storage with explicit image axes and structural component groups. Component groups name channel ranges for storage grouping; the kernel does not derive a color model, alpha, or numeric range from them. Its virtual address reservation, backed pages, metadata capacity, and valid samples are distinct quantities. Continuous rows and tiled rows store contiguous samples within each row. Tiled geometry requires positive power-of-two tile height and width; image and ROI extents may end at partial tiles.

Pages become readable only after their produced samples are published. Published samples are immutable, and storage leases remain until the last image or read-window owner retires. Budget exhaustion does not evict live backing. `acquire` returns a retained read window; `row_run` and `rectangle_run` expose only authorized spans, bounded by the requested Region and physical tile. `read` copies into caller-owned packed storage. A transactional write window publishes authorized output spans on commit and discards unpublished writes on failure.

## 3. Scheduling and state

```text
WorkflowDocument -> declarations + inferred schemas ----> ExecutionPlan
          |                                              |
          +---- caller-owned named bindings ------------+
                                                         v
                                                    ExecutionRun
                                           +-------------+-------------+
                                           |                           |
                                  Result producers and typed tensor slots
                                           |                           |
                                  named Result outputs      dependency evidence
                                           +-------------+-------------+
                                                         v
                                                ExecutionResult
```

`ExecutionBindings` uses exact names and supplies Result owners for structured operation inputs. Binding checks the declaration schema before callbacks. The plan does not capture runtime addresses, so separate immutable bindings can execute the same current plan independently or concurrently. `Value` and `PlanarImage` may provide local or backing storage, but they are not alternate structured operation ports.

Planning retains two closures. The metadata closure follows every static input of output-reachable operations so schemas, specialization, and identity remain complete. The executable closure starts at named outputs and side-effect roots, then follows only each selected output's `input_indices`; when an output does not declare a projection, all of that node's inputs are executable. Backend placement and admission apply to the executable closure. An input used only for metadata still undergoes static validation and contributes to identity, but its producer does not run. A directly requested GPU-only output remains unavailable in `CpuExact` mode.

Workflow input declaration ids and node ids use separate namespaces. A document may contain up to 4096 declarations; each declaration has a unique nonzero id and unique 1-to-128-byte printable ASCII name (`0x21` through `0x7e`). Compiler stages carry declarations in id order. Every Result input declaration carries its complete schema; a missing schema is rejected with `InvalidArgument`. The compiler resolves input-derived extents from static input domains without reading payload samples.

When a Result binding references storage outside the execution root, the runtime admits its retained capacity under `Referenced`. A published view keeps its source Result and backing owners alive. Result tensor reads use the captured descriptor and the requested authorized region; see [Structured Results and tensor slots](Global-Results.md) for publication and window ownership.

`ExecutionResult` contains named Result outputs, dependency evidence, and diagnostics. Each returned Result retains the leases needed by its storage until its final owning reference is released.

## 4. Algorithms and validation

Result schemas describe logical sample domains; the compiler does not require their dense element product or packed byte count to fit a machine allocation size, and Result planning does not estimate dense payload bytes. Actual `Value` backing creation validates allocation size and addressed byte span, and its strides must fit the signed representation. A Result may describe a much larger logical domain than its physical backing, for example a broadcast view over a small allocation.

Result tensor schemas describe logical axes and optional spatial layout. Physical tiled addresses are not represented by ordinary affine strides. Each operation declares supported Result schemas and execution behavior; `PlanningOptions` supplies physical tile geometry where spatial planning uses it.

`SchemaTemplate::id` and `version` identify an explicit structural contract and enter compile identity. The kernel does not dispatch on them to a domain interpretation. Two Results with the same structure but different meanings for the caller, for example linear and sRGB-encoded RGB, connect to the same port; the operation's explicit parameters and bound validators decide what the samples mean.

Float32 Values preserve binary32 payload bits. A scalar or operation contract validates any numeric domain it consumes; storage accepts the element representation independently from that domain. Domain schemas such as the 3D LUT bake report and table are defined by the operations SDK (`photospider/ops/numeric/lut3d_bake.hpp`), not by the kernel; see [CRV-09](../built-in_ops/01-numeric/op_specs/CRV-09_bake_lut3d.md).

## 5. Limitations and non-goals

- Results are in-memory values. They have no durable identity, receipt, serialization, or recovery contract.
- The data-definition registry stores copied schema metadata from trusted startup registrations or DSOs; it does not allocate Values or provide storage.
- Structural image support is declared by the Result tensor schema and operation contract; an unsupported combination returns a typed failure during validation or planning.
- `CpuStorage` exposes CPU-accessible immutable bytes and may retain completed native storage. Device handles remain private.
- The pipeline element types are UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) D6 (Proposed) keeps this set closed and rejects unknown dtypes.
- The kernel has no time or sequence type. Batch axes, including the image frame/layer axes, carry no temporal meaning; ADR 0022 D7 assigns time representation, frame selection and cross-frame state to the host application.
