# Data Model

## 1. Scope and ownership

The compiler owns immutable descriptions of graph structure and inferred metadata. Each execution receives named input owners and creates runtime Values or PlanarImages. `ExecutionResult` owns the requested named outputs and diagnostics; returned storage remains alive through shared owners and resource leases.

## 2. Core structures and memory layout

```cpp
struct WorkflowDocument {
  std::uint32_t schema_version = 3;
  std::vector<WorkflowInputDeclaration> inputs;
  std::vector<WorkflowNode> nodes;
  std::vector<WorkflowOutput> outputs;
};

class Region;  // logical offset/extent intervals in descriptor-axis order

class Value {
 public:
  static Result<Value> create(ValueDescriptor, Region, StridedLayout,
                              std::vector<std::uint8_t>,
                              std::vector<ValueFacet> = {},
                              ResourceBindings = {});
};

struct ExecutionBinding {
  std::string name;
  Value value;
  std::shared_ptr<const RegionalSource> source;
  std::shared_ptr<const InputSnapshot> snapshot;
  std::shared_ptr<const PlanarImage> image;
};

class PlanarImage {
 public:
  static Result<PlanarImage> create(ValueDescriptor, PlanarImageConfig,
                                    std::vector<ValueFacet> = {},
                                    ResourceBindings = {});
  static Result<PlanarImage> import_value(const Value&, PlanarImageConfig,
                                          const CancellationToken& = {});
};
```

`WorkflowDocument` is copied compiler input. Its nodes carry operation keys, ordered input references, typed parameters, and named output selections. Input declarations describe fixed metadata; execution bindings provide the corresponding payload or image owner. Plans keep declaration metadata, not caller pixel addresses.

A generic `Value` has a nonzero rank-1-to-8 descriptor, a logical `Region`, a `StridedLayout`, up to 64 unique facets, and shared immutable CPU-accessible storage. Its element types are `UInt8`, `Int8`, `UInt16`, `Int16`, `Int64`, `Float32`, and `Float64`. `Value::create` validates shape and coverage, stride count and addressed byte span, element type, and facet/resource consistency before publishing. Negative and zero strides are valid when every addressed byte remains in the backing allocation. Copies share storage and expose no writable pointer.

`Region` stores unsigned offset/extent pairs in descriptor-axis order. It describes logical samples, not bytes. Region interval and element-count arithmetic is checked. `Value::as_float64()` accepts only a contiguous Float64 scalar whose Region is exactly rank one with `{offset=0, extent=1}`; other valid Value coverage remains usable through the general accessors.

`BufferAllocator` reserves capacity before allocation. Move-only mutable buffers transfer their lease into immutable storage at publication. `Value::from_storage` and `view` can share backing without copying; `bytes()` returns a borrowed view and `copy_bytes()` makes an explicit caller-owned copy. Storage can outlive the allocator and execution context. Regional-source callbacks fill a requested packed Region synchronously; their writable destination and allocator pointers expire when the callback returns.

`PlanarImage` represents structural rank-two or rank-three storage with explicit image axes, component groups, facets, and resources. Its virtual address reservation, backed pages, metadata capacity, and valid samples are distinct quantities. Continuous rows and tiled rows store contiguous samples within each row. Tiled geometry requires positive power-of-two tile height and width; image and ROI extents may end at partial tiles.

Pages become readable only after their produced samples are published. Published samples are immutable, and storage leases remain until the last image or read-window owner retires. Budget exhaustion does not evict live backing. `acquire` returns a retained read window; `row_run` and `rectangle_run` expose only authorized spans, bounded by the requested Region and physical tile. `read` copies into caller-owned packed storage. A transactional write window publishes authorized output spans on commit and discards unpublished writes on failure.

## 3. Scheduling and state

```text
WorkflowDocument -> declarations + inferred descriptors -> ExecutionPlan
          |                                              |
          +---- caller-owned named bindings ------------+
                                                         v
                                                    ExecutionRun
                                           +-------------+-------------+
                                           |                           |
                                     generic Value              PlanarImage
                                           |                           |
                                  named result Value          named result image
                                           +-------------+-------------+
                                                         v
                                                ExecutionResult
```

`ExecutionBindings` uses exact names. Ordinary generic execution accepts one of `Value`, `RegionalSource`, or `InputSnapshot` for a generic input; a planar declaration selects `image`. In planar execution, a non-planar declaration accepts a matching `Value` only, while a planar declaration requires `PlanarImage`. Execution checks declaration names and metadata before callbacks. Image binding also checks descriptor, facets, structural layout, and plan tile geometry. The plan does not capture runtime addresses, so separate immutable bindings can execute the same current plan independently or concurrently.

Workflow input declaration ids and node ids use separate namespaces. A document may contain up to 4096 declarations; each declaration has a unique nonzero id and unique 1-to-128-byte printable ASCII name (`0x21` through `0x7e`). Compiler stages carry declarations in id order. Each declaration fixes a complete descriptor, whole Region, canonical dense input layout, and exact facet set; planar declarations use `planar_layout` with an empty affine layout.

During planar execution, the Run pins external image owners against publication and admits their stable resident capacity. Repeated references to the same owner and same-context result rebinding share one accounting entry. Ordinary read windows still permit publication to disjoint regions. See [Tensor storage and region access](../kernel-specs/Tensor-Storage-and-Region-Access.md) for page, tile, and publication geometry.

`ExecutionResult` can contain named generic Values, named PlanarImages, supported structured results, and diagnostics. Planar images have no implicit dense Value export. Each result owner retains the leases needed by its storage until its final release.

## 4. Algorithms and validation

For a dense generic input, the compiler validates the packed byte count using checked products without allocating payload. The byte count must be positive, the last addressable byte must fit `INT64_MAX`, and the total must fit `SIZE_MAX`; stored row-major strides must fit their signed representation. General runtime Values can use non-dense strided layouts and partial Regions when their addressed span fits storage.

An image input declaration carries `planar_layout`; its affine layout is empty because tiled addresses cannot be represented by ordinary strides. The layout describes axes, storage mode, row pitch, and component groups. Each operation declares planar output support independently. `PlanningOptions` supplies one DAG tile geometry, which execution checks against bound images.

Float32 Values preserve binary32 payload bits. A scalar or operation contract validates any numeric domain it consumes; storage accepts the element representation independently from that domain.

## 5. Limitations and non-goals

- Results are in-memory values. They have no durable identity, receipt, serialization, or recovery contract.
- The data-definition registry stores copied schema metadata from trusted startup registrations or DSOs; it does not allocate Values or provide storage.
- Structural planar callback support is operation-trait and entry-point specific. An unsupported combination returns a typed failure before legacy image code can run.
- `CpuStorage` exposes CPU-accessible immutable bytes and may retain completed native storage. Device handles remain private.
