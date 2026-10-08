---
spec_schema_version: 1
id: KERNEL-tensor-storage
kind: shared_kernel_contract
status: Accepted
implementation_status: implemented_cpu
---

# Tensor storage and region access

This specification defines the storage and bounded access contract for tensors owned by `Result`. [The Chinese reader version](zh/Tensor-Storage-and-Region-Access.zh.md) follows this document. `ResultTensorSpec` defines logical shape, sample grouping and optional spatial topology; the backing may use private planar pages or affine CPU storage. Color and alpha meaning belongs to tensor facets, while byte layout and region access belong here.

## Scope and ownership

`SchemaTemplate::tensors` is the schema collection for rank-1..8 tensor cells. A cell has a `ValueDescriptor` for its element type and cell shape, zero or more explicit `batch_axes`, an optional `atomic_trailing_axes` grouping and a `ResultTensorLayout`. `ResultRef` owns schema, certified sample coverage, relations, retained source associations, resources and backing lifetime. `PlanarImage` is an internal backing implementation for spatial tensors; it does not define another semantic result.

`ValueDescriptor`, `StridedLayout`, `CpuStorage` and the internal `Value` representation provide scalar and affine-storage machinery. They do not change which `Result` slot owns the samples or which regions are certified. `ResultTensorSpec::sample_shape()` prepends the declared batch axes to the cell shape. An ordinary numeric tensor can have no batch axes; the schema does not invent frame or layer axes. Spatial backing maps the declared height, width and optional channel axes in the cell shape.

The rank limit applies to the full sample shape. Cell rank is 1..8, batch-axis count is 0..7, and their sum is at most 8. Every tensor cell and batch-axis extent is positive. Validating a logical schema does not require a dense allocation or a representable product of all extents; operations that need the total count use checked `sample_count()` and may receive a resource error for an overflowing product. Schema validation alone cannot predict whether a later request for planar backing will pass its reservation and page-budget checks.

### Metadata-derived Result outputs in C++ registrations

A C++ operation can declare a tensor-member predicate for a Result port and leave the constraint's schema id/version unresolved. For output constraints this requires metadata specialization and a nonempty member predicate: `tensor_key` may name a member, or an omitted key requires exactly one tensor. The registered `OperationOutputTraits::result_schema` prototype remains complete. The specializer returns the concrete schema; the registry validates it and resolves its id/version into the output constraint. Fixed-schema registrations preserve their declared id/version. The C ABI continues to require fixed output schemas.

`core.identity` specializes its output to the input schema and accepts exactly one tensor with no scalar fields, any member key/schema id, and one of the seven supported dtypes. It preserves schema id/version, member key, batch axes and facets. It declares bitwise-mapped movement; execution can retain a view when the mapping is available or materialize a copy when it is not. The output association records the actual input ObjectId; identity publishes a new Result ObjectId. Its input read uses role 13 (Data, Validation and Descriptor), and its mapped relation uses role 5.

## Core data model and memory

```cpp
struct ResultTensorLayout final {
  bool spatial = false;
  ImagePlaneOrder order = ImagePlaneOrder::Tiled;
  std::uint32_t height_axis = 0, width_axis = 1;
  std::optional<std::uint32_t> channel_axis = 2;
  std::uint64_t row_pitch_bytes = 0;
  std::vector<ImageComponentGroup> groups;
};

struct ResultTensorSpec final {
  ResourceLease metadata_owner;
  ResourceString key;
  ResourceVector<std::uint64_t> batch_axes;
  std::uint32_t atomic_trailing_axes = 0;
  ValueDescriptor descriptor;
  ResultTensorLayout layout;
  std::vector<ValueFacet> facets;

  std::vector<std::uint64_t> sample_shape() const;
  std::vector<std::uint64_t> observation_shape() const;
  Result<std::uint64_t> sample_count() const;
  Result<Footprint> close_samples(
      const Footprint& samples, const FootprintLimits& limits = {}) const;
};

struct SchemaTemplate final {
  ResourceString id;
  std::uint32_t version = 1;
  PublishPolicy publication = PublishPolicy::CompleteBundle;
  ResourceVector<ResultFieldSpec> fields;
  ResourceVector<ResultTensorSpec> tensors;
  ResourceVector<ResultExtent> domain;
  ResourceVector<ResultFacet> metadata;
};
```

The excerpt omits validation, managed-copy and canonical-identity methods. `ResultFieldSpec` remains for bounded primitive records; tensor samples use `SchemaTemplate::tensors`. Physical order and row pitch do not participate in semantic schema identity. Two slots with the same logical schema can therefore use different valid CPU backing strategies.

`atomic_trailing_axes` makes the specified trailing cell axes one observation: `close_samples()` expands each non-empty request across those axes. Recognized semantic Image and ColorArray facets also close over their complete channel tuple. Tensor description metadata and physical `groups` alone do not create peer-channel demand. The operation's declared sample Need remains the authorization for reading samples.

The logical batch axes can represent any declared context, including frame/layer when the application schema uses those names. Non-spatial tensors have no frame/layer interpretation imposed by the storage layer. Spatial batch domains are represented by their logical extents; schema validation has no fixed 4096-product ceiling, and Result does not allocate a dense backing directory for the full product. Concrete reads, relation construction and backing requests still use their checked arithmetic and resource limits.

### Affine backing

An affine `StridedLayout` supplies one signed byte stride per logical axis and an optional logical origin for the byte offset. Positive, negative and zero strides are accepted when checked address bounds keep every sample inside the retained `CpuStorage` span. Zero stride represents broadcast samples. A singleton axis does not consume address span, regardless of its stride value. The storage span includes the final element and is checked before publication; padding bytes are not samples.

The layout can describe a very large logical domain with a small physical span. For example, shape `{INT64_MAX+2, 3}` can use strides `{0, -1}` over three bytes: every coordinate on the first axis broadcasts, and the second axis walks backward. This is valid because the logical extent is not used as a dense allocation request. Address and offset arithmetic still must pass signed and unsigned overflow checks.

`ResultTensorReadWindow::row_run()` returns a borrowed run whose `data` points at the first logical sample and whose `sample_stride_bytes` may be negative or zero. `bytes` is the checked address span including the final element, not a promise that `data[0..bytes)` is contiguous logical data. Use the stride for each sample. `rectangle_run()` adds a signed row stride; it does not imply that rows are contiguous.

### Spatial backing

`layout.spatial` selects the spatial access convention. Height and width axes identify cell-local coordinates; channel is optional. A private `PlanarImage` reserves one continuous virtual address span for the coordinates it backs. Each logical coordinate has a stable byte offset in that span, while physical page backing is supplied on demand. `ResultBuilder::start()` stores the schema, empty coverage and relation, and physical configuration; it does not reserve a planar span for every logical batch coordinate. Affine `CpuStorage` publication and reads do not create planar backing. The same `ResultTensorReadWindow` run axes and coordinates are used regardless of backing.

For materialized planar rows, adjacent samples have positive scalar-width stride. Continuous storage may add checked row-end padding. Tiled storage uses row pitch `Tw*d`, where `d` is the element width; a tile edge may stop a run before the requested region ends. A regular planar HWC tensor with tightly packed planes can be represented by strides `{W*d, d, H*W*d}`; the equivalent CHW order is `{H*W*d, W*d, d}`. Physical planar storage does not require logical CHW order. General affine storage may have signed or zero strides and does not automatically become a planar backing.

`PlanningOptions` supplies one common tile height and width for the DAG plan; an operation does not select a separate output tile size. See the [parallel execution model](../kernel-architecture/Parallel-Execution-Model.md) and [`PlanningOptions`](../../include/photospider/compiler/compiler.hpp). `ResultBuilder::start()` validates positive power-of-two tile extents, including 1, before accepting the physical configuration. The planar reservation size, page limits and address arithmetic are checked when an operation first requests that backing. The current default is 128 by 128. The logical image and ROI extents may be arbitrary positive values. The formulas below describe private tiled backing only; they do not define semantic coordinates or make padding valid.

The [FMT common contract](../built-in_ops/02-format-color/op_specs/FMT_common_contract.md) and [codec boundary](../built-in_ops/02-format-color/op_specs/FMT_codec_boundary.md) define color metadata and image import/export responsibilities. Internal color and alpha planes use matching dimensions and co-sited samples, including full-resolution Y, Cb and Cr. External chroma subsampling and physical packing belong to codecs. These constraints do not prohibit affine CPU backing for a tensor.

## Tile layout and checked arithmetic

Let tile size be `(Th,Tw)`, a plane have extent `H x W`, scalar width be `d`, and the host page size be `P`. The grid starts at image coordinate `(0,0)`, independent of the requested ROI. Coordinate `(y,x)` belongs to tile `(floor(y/Th), floor(x/Tw))` and local offset `(y mod Th, x mod Tw)`.

For a tile with valid extent `ht x wt` and byte offset `ot`:

```text
row_pitch      = Tw*d
valid_bytes    = ht*wt*d
padding_bytes  = ht*(Tw-wt)*d
storage_span   = ht*Tw*d
next_offset    = align_up(ot + storage_span, P)
alignment_gap  = next_offset - (ot + storage_span)
```

An interior tile stores a tight `Th*Tw` block. A right edge pads the end of each valid row. A bottom edge retains only its valid rows; it does not reserve the missing `Th-ht` rows. Thus a `2 x 72` edge for `Tw=128` occupies `2 x 128` sample slots, not `128 x 128`. Every tile start is page-aligned, even when its payload is smaller than a page. The image base is page-aligned; platform reservation rounding may add a tail.

Let `nx=ceil(W/Tw)`, `q=floor(H/Th)` and `hrem=H mod Th`. Each tile row shares a valid height. Checked layout terms are:

```text
full_step    = align_up(Th*Tw*d, P)
edge_step    = 0 if hrem=0 else align_up(hrem*Tw*d, P)
plane_step   = nx*(q*full_step + edge_step)
ht           = min(Th, H-ty*Th)
tile_offset  = plane_offset + ty*nx*full_step
               + tx*align_up(ht*Tw*d, P)
sample_offset(y,x) = tile_offset + (y mod Th)*Tw*d + (x mod Tw)*d
```

For equal-sized planes, `plane_offset=c*plane_step`. The reserved span includes aligned tile slots. Only `ht*Tw*d` bytes inside a tile slot belong to its row-padded sample storage. Page alignment gaps and row padding are not samples. The number of tiles is `ceil(H/Th)*ceil(W/Tw)`; byte counts, pitches, offsets, alignment and shape products are checked before allocation.

Padding is not an implicit zero, an image sample, or a filter boundary rule. It does not change color values, sample identity or dirty-region mapping. In continuous backing, a page may span plane boundaries. In page-aligned tiled backing, a page cannot contain the next tile, but can also cover unrequested positions in the same tile. A page being provided does not certify its other samples.

### Row-padded edge example

For `W=200`, `H=130`, `C=4`, Float32, `T=128` and page/reservation granularity `P=16384`, the first plane has these tiles (offsets and pitches are bytes):

| Valid height | Valid width | Row pitch | Tile offset |
| --- | --- | --- | --- |
| 128 | 128 | 512 | 0 |
| 128 | 72 | 512 | 65536 |
| 2 | 128 | 512 | 131072 |
| 2 | 72 | 512 | 147456 |

The next plane begins at 163840. Four planes reserve 655360 bytes (640 KiB): 416000 valid sample bytes, 116480 row-padding bytes, and 122880 alignment-gap/tail bytes. Tile storage including row padding totals 532480 bytes (520 KiB). R at `(y=129,x=199)` has byte offset 148252. With no existing backing, preparing this sample provides page index 9, or 16384 bytes, while the logical request remains four bytes. Other pixels in that page remain uncertified.

## Read windows and transactional writes

```cpp
struct ResultTensorRun final {
  const std::uint8_t* data = nullptr;
  std::uint64_t samples = 0, bytes = 0;
  std::int64_t sample_stride_bytes = 0;
};

struct ResultTensorRectangle final {
  ResultTensorRun row;
  std::uint64_t rows = 0;
  std::int64_t row_stride_bytes = 0;
};

Result<ResultTensorReadWindow> ResultRef::acquire_tensor(
    const ResultDescriptor&, std::uint32_t slot, const Region&,
    const CancellationToken&) const;

Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Region&,
    const std::function<Status(
        const ResourceVector<ResultTensorWriteWindow>&)>& write,
    ResultRelation, ResultFinality, const CancellationToken&);
```

The complete declarations are in [`result.hpp`](../../include/photospider/data/result.hpp). A read window owns the source `ResultRef`, captured descriptor facts, schema, association, resources and backing needed by its exact authorized region. It is move-only. Its run pointers remain valid until the window is destroyed. A captured descriptor limits the observable prefix: a later publication cannot expand an earlier descriptor's coverage. A request outside certified coverage returns `NotFound`; stale descriptor facts return `Stale`.

A read window authorizes one logical rectangle and may span several batch coordinates when the full rectangle is certified. For private planar backing, acquisition retains a separate physical piece for each covered batch prefix; each piece records its complete batch coordinate. `row_run()` and `rectangle_run()` address one coordinate at a time: spatial runs advance along the cell width axis and rectangles along the cell height axis, while the caller supplies the batch coordinates. Thus one window can cover many batches without making an individual physical run cross a batch boundary. Non-spatial windows can cover a general region; runs advance along the final axis and rectangles along the penultimate axis when present.

The coordinator authorizes the exact logical sample set before constructing the window. For affine storage it retains subviews of the admitted `CpuStorage`; for private planar backing it retains page windows, including when a certified view maps samples to source pages. It does not expose reserved address ranges or padding as readable data. Batch-piece index comparisons consume work from the source root. The window retains the acquisition cancellation token, and later run lookups return `Cancelled` after that token is cancelled; a pointer already returned remains valid until the window is destroyed. Page provision can exceed the requested bytes and is charged separately from logical sample coverage. The coordinator prepares pages and upstream samples explicitly before the callback; page-fault handling does not dispatch DAG work. It neither evicts live pages automatically nor replays a producer to recover them. Sparse bookkeeping records selected pages and does not preallocate a metadata record for every possible page in the virtual span.

`publish_tensor_kernel()` gives the callback write access only to the requested unpublished region. The callback's borrowed windows and pointers expire when it returns; concurrent work must write disjoint runs and finish before return. Generic tensors use one owned payload allocation and publish that storage directly after callback success. Spatial tensors prepare private destination pages before invoking the callback. For each requested batch coordinate, the builder reuses an installed `PlanarImage` or prepares a private candidate and records only that coordinate in its sparse backing directory. It installs new candidates only after every writer succeeds; failed preparation or callback work releases candidate pages, virtual reservations and metadata while leaving old certified prefixes available. If the requested planar span cannot be reserved, publication fails before the callback runs. Empty regions skip the callback and sample payload/page work and add no sample coverage; descriptor relation obligations remain represented by the Result.

The spatial write path is transactional. It prepares all required pages and capacity before callback execution, commits only after successful callback completion and cancellation checks, and then certifies the exact region and relation. Callback failure, cancellation, overlap or resource failure certifies no new sample coverage; previously certified prefixes remain readable and immutable. Allocation/provision failure rolls back unpublished pages. An admission failure returns a typed status such as `ResourceExhausted`; live published pages are not evicted or silently recomputed.

While this callback is active, same-producer mutation and sealing are rejected. Moving or replacing the builder fails the active transaction before it can commit. The producer object must remain alive until `publish_tensor_kernel()` returns. Reading an already-certified prefix and using independent builders remain supported. A failed publication records the producer failure; later publication attempts observe that failure. The writer does not grant access to future samples merely because they share a page or backing allocation.

## Mapping views

```cpp
struct ResultTensorViewTransform final {
  std::vector<ResultMappedAxis> source_axes;
  bool reshape = false;
};

Status ResultBuilder::publish_tensor_view(
    std::uint32_t slot, const Region& target,
    const ResultTensorReadWindow& source,
    const ResultTensorViewTransform& transform,
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation = {});
```

`ResultBuilder` has two view-publication forms. The ordered-window form assembles consecutive physical planar planes from one root, with matching element type, spatial coordinate rectangle, order and pitch. The transform form maps one logically authorized affine source window to an output region. That window may contain multiple affine fragments when they share one `CpuStorage` owner and together describe one affine address map. The kernel infers strides for singleton axes from neighboring logical coordinates, then verifies the inferred map against every fragment. Both forms retain the source `ResultRef`, captured facts and resource owners, publish only authorized target samples, and do not copy payload bytes. Source payload remains charged to its owner root; the target charges its own metadata and relation. The affine transform path can publish when the target has no payload capacity. Source-to-target cycles are rejected.

For an axis transform, `source_axes` has one [`ResultMappedAxis`](../../include/photospider/data/result_relation.hpp) for every source-window axis, including batch axes. Each map selects exactly one source coordinate (`extent == 1`). Its output axis may be fixed (`output_axis == -1`) or may refer to a target axis in `0..target_rank-1`. For mapped axes, coordinates follow:

```text
source_coordinate = source_origin
                    + step * (target_coordinate - output_origin)
```

The calculation is checked over the full unsigned coordinate range. Positive and negative steps select forward and reversed slices; zero step broadcasts one source coordinate. The output axes can be reordered, inserted, dropped or shared by multiple source-axis maps. The source window's exact region remains the authorization boundary.

With `reshape == true`, `source_axes` is empty. The transform flattens the complete source window in logical row-major order, then assigns that order to the complete output shape. The relation uses the complete output shape as its ordinal basis: an output ordinal is unraveled against the source-window extents, then each local coordinate is translated by the corresponding source-window offset in the larger input tensor. This logical mapping is independent of the source's physical layout. Signed and zero strides affect whether the requested payload can be represented as a view, but do not change dependency support.

`ResultRelation::reshape()` constructs the matching compact witness. Both shapes have rank 1 through 8, and the factorized cardinalities must match without forming a `uint64_t` dense element product. The output `Region` is the witness; samples outside it have no support. `project(Q)` clips a requested output footprint `Q` to that witness, maps its exact boxes into source coordinates, and emits the typed tensor support. `preimage(Q, changed)` computes the exact dirty subset within `Q`; `certify()` checks witness coverage. Empty requests and empty witnesses produce no dirty samples. Holes remain exact sets of boxes rather than expanding to a bounding box. If exact projection exceeds rectangle or work limits, cancellation occurs, or metadata/scratch admission fails, the operation returns an error without visiting with a partial projected set. Scratch boxes are allocated against the relation's resource root; work is charged either through the supplied `consume_work` callback or directly to that root, once per unit.

The C ABI 2 service [`ps_result_services_v2::publish_tensor_view`](../../include/photospider/plugin/result_operation_plugin_api.h) uses a nullable `ps_result_tensor_transform_v2`. `NULL` selects the ordered planar assembly and may provide several source window handles. A non-NULL transform selects the affine form and requires exactly one source window. With `reshape == 0`, `axes` contains one anchored point map per source axis. With `reshape == 1`, `source_rank` is zero and `axes` is NULL. The C transform object is borrowed for the service call; successful publication retains the source independently of its window handle.

For the affine transform form, `ViewUnavailable` means the source backing cannot express the requested map, including page-backed or non-affine source windows and affine fragments with different owners. The ordered-window form separately supports compatible planar pieces. An affine `ViewUnavailable` leaves the builder usable for an explicit materializing copy; the numeric `auto` layout may take that path, while `view` reports the unavailable mapping. Resource admission or work-limit errors, cancellation, invalid transforms, source authorization, incomplete channel closure, target overlap, finality, relation ownership or coverage, and cycles remain errors and do not select a copy retry. A wrong `2`-to-`3` reshape is therefore an `InvalidArgument` even when the source would also be physically fragmented.

The behavior tests exercise six C++ affine cases: transpose, negative-step slice, reverse reshape, a `{UINT64_MAX, UINT64_MAX}` zero-stride reshape, a singleton axis with `INT64_MIN` stride, and broadcast. They also verify compact reshape projection, inverse dirty regions, witness coverage, huge factorized shapes, exact holes, cancellation and resource-limit failures. A C11 DSO fixture exercises `make_reshape()` and reshape view publication for a `2x3` source mapped to `3x2`, using both negative and zero strides. It checks output samples, exact dirty mapping, pointer and owner identity, and reads a retained window after the source capability and binding retire. `make_reshape()` constructs dependency support; it does not grant payload-read authorization.

## Resource accounting and release

Track virtual reservation, provided page capacity, valid sample coverage and charged metadata separately. A small logical view can retain a larger set of backing pages. The resource root accounts for output storage, windows and metadata under their respective limits. `resident_bytes()` reports an accounting snapshot of backing and metadata; it is not measured process RSS or a guarantee that the operating system keeps every page resident.

`ResultRef::association()` records the validated, ordered source object IDs, and its dependency bundle preserves source observations without retaining input payload. A materialized or scalar-copy output can therefore outlive its inputs while keeping its source identities and dependency facts. A published affine or planar view separately retains each source `ResultRef` whose bytes it uses, along with that source's schema, resources and backing. The last owning Result or retained window releases those view-source owners; closing a read window does not revoke published samples while the Result remains alive. Resources named by tensor facets remain retained with their Result owner. Cancellation or failure releases unpublished allocations normally while preserving already-published samples and observations.

## Limits and non-goals

- The schema rank limit is eight logical sample axes. Spatial backing is created only for requested batch coordinates; it does not require a dense directory for the logical batch domain.
- The current private planar geometry requires positive power-of-two tile height and width. It does not support per-plane tile sizes.
- Physical layout groups describe organization. They do not authorize sample reads, validate color values or create channel dependencies by themselves.
- A provided page does not imply valid sample coverage for the rest of that page. Missing samples are not zeros.
- Affine source-window publication supports anchored point maps and compatible signed/zero-stride reshape chunks. C ABI `make_reshape()` supplies compact dependency support, while payload access still requires an explicit tensor window authorization.
- This document defines CPU tensor storage and access. It does not promise GPU address mapping or physical residency.
- Package, workflow, traits and plugin ABI version values are maintained in the [Compiler Version Contract](../development/Compiler-Version-Contract.md).

## Behavioral evidence

The current registered image tests are grouped by behavior. [`test_result_image_backing.cpp`](../../tests/integration/image/test_result_image_backing.cpp) covers tensor windows, sparse batch access, publication, backing boundaries, and retained owners. [`test_result_image_views.cpp`](../../tests/integration/image/test_result_image_views.cpp) covers affine and fragmented views, while [`test_result_image_transform.cpp`](../../tests/integration/image/test_result_image_transform.cpp) covers split and downsample behavior. [`test_result_image_format.cpp`](../../tests/integration/image/test_result_image_format.cpp) covers image schema and controls. Shared Result contract checks are in [`test_result_image_contracts.cpp`](../../tests/integration/test_result_image_contracts.cpp), with C-plugin service and publication cases in [`test_result_plugin.cpp`](../../tests/integration/test_result_plugin.cpp). These sources cover schema rank and atomic grouping, huge logical extents with compact signed/zero-stride backing, sparse windows with bounded work and cancellation, tile boundaries, exact read authorization, transactional writes, planar assembly, affine transforms, and access after source-window or context retirement. They do not establish every plugin transformation or native GPU mapping.

Page reservation and commitment are distinct operating-system concepts; [Microsoft's `VirtualAlloc` reference](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc) describes that distinction for Windows. The accounting terms in this specification describe the kernel's managed backing, not portable RSS behavior.
