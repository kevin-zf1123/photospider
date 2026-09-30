---
spec_schema_version: 1
id: KERNEL-tensor-storage
kind: shared_kernel_contract
status: Accepted
implementation_status: implemented_cpu
clarification_status: selected_storage_policy_complete
---

# Tensor storage and region access

This is the CPU storage contract in package 0.29.0 and WorkflowDocument schema 4.
[Chinese reader version](zh/Tensor-Storage-and-Region-Access.zh.md). It owns
physical layout, tile geometry, and region access. A structured `ResultRef` is
the sole semantic, publication, input/output, and lifetime owner for image data;
`PlanarImage` is private typed backing inside its image slots. It does not create
a second public image result or execution path. [FMT-common](../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
owns associated color/alpha interpretation. This contract does not preserve the
retired image memory/numeric interfaces or provide compatibility adapters.

## Confirmed decisions

| Topic | Selected target |
| --- | --- |
| Image layout | Every image is planar. Interleaved imports require explicit I/O codec layout conversion before entering the kernel. |
| Graph tile policy | One DAG-wide tile size; operators cannot choose different output tile sizes. Both extents must be positive powers of two (including 1); non-power-of-two tiles are unsupported. Examples include 64x64, 128x128 and 256x256. |
| Plane organization | Continuous planar storage remains available; when tiled, all tiled image planes follow the DAG geometry, with no per-plane size override. |
| Backing | Reserve one full-image continuous virtual address span; provide backing by page as needed. Plane/tile access retains the shared image address-space owner, rather than unrelated image allocations. |
| Rows | Samples within a plane/tile row are contiguous; row-end padding is permitted. |
| Color and alpha | One tensor may contain a color group and an independent alpha plane, including L*,a*,b*,Alpha. Alpha is not another Lab color coordinate. |
| Incomplete edge tiles | Retain only valid rows and pad each row to the common tile width. Interior payloads remain tight full blocks. Every following tile starts on a host page boundary; gaps are separate from row padding. No added bottom rows. |
| Page preparation | Explicitly acquire/prepare required pages before running the operator, with resource/cancellation checks. Do not use page-fault handling to dispatch DAG computation. |
| Page retention | Retain produced page backing until the image's final lifetime owner retires; fail on budget exhaustion. No automatic eviction, DAG replay or temporary-file paging. |

Graph-wide geometry remains the selected tile policy. Each typed image slot owns
one planar backing per frame/layer pair; each backing uses one continuous image
virtual range with pages provided on demand. A typed Result image slot is the
semantic image carrier. The former Image/Layer Value special cases remain
retired. Generic tensor shape/dtype, physical layout, color groups and consumed
metadata remain separate. Raw/override does not change storage addresses or
turn an interleaved import into planar storage by relabeling it.

The [codec-boundary clarification](../built-in_ops/02-format-color/op_specs/FMT_codec_boundary.md)
requires same-size, co-sited color/alpha planes within an image, including
full-resolution Y/Cb/Cr. External chroma subsampling and physical layout/packing
belong to input/output codecs; FMT-16/17 are retired. This clarifies the image
boundary and does not claim a codec implementation or change the storage
addressing formulas below.

## Logical coordinates and layout

Logical axis order remains explicit: physical planar storage does not force
every tensor to use CHW. A logical HWC tensor can describe continuous planar
storage with byte strides [W*d,d,H*W*d] when there is no padding and all planes
are packed together; d is the element width. A CHW description of those bytes
has strides [H*W*d,W*d,d]. Source/target shape and axis transformations are checked
independently of color interpretation.

For materialized image storage, adjacent row samples have positive byte stride
d. A continuous plane may have checked row-end alignment padding. In tiled mode,
every row pitch is exactly Tw*d, including right-edge tiles. Row-width padding
and tile-start page alignment both apply, at their respective granularities.
Strided numeric views remain generic tensor capabilities;
in particular, a red view with pixel stride 4*d into interleaved RGBA does not
meet this standard image storage requirement. Publishing it as an image requires
the explicit physical conversion. Ordinary ROI views of planar rows retain their
owner and row pitch without requiring full-row copying.

One tensor retains its uniform dtype and shape relationships. A color group and
alpha can share that carrier while retaining independent semantic roles. This
storage contract defines no premultiplied Lab and does not apply a color transfer
to alpha. Semantic Image and ColorArray facets preserve their existing complete
channel-tuple closure, including alpha when it is a channel. TDM-only facets and
`PlanarImageLayout::groups` describe metadata or physical organization and do
not add peer-channel or alpha sample demand. An operation's declared sample
needs remain the dependency authority.

For a structured image slot, each backing descriptor keeps its declared axis
order. `{H,W}` and `{H,W,C}` are common examples, not required axis orders. A
CHW backing uses descriptor shape `{C,H,W}` and maps channel, height, and width
to descriptor axes 0, 1, and 2. Result logical coordinates prepend frame and
layer, so that backing appears as `{N,L,C,H,W}` to sample requests. `N` and `L`
are positive and `N*L` is bounded by 4096. The slot schema retains frame/layer
identity, while plane and tile formulas apply independently to each backing.
Frame and layer are not flattened into color channels.

## Continuous and tiled planes

In continuous mode, each plane's rows occupy their declared row-pitched segment
of the common backing. Plane order follows the declared channel order, with
explicit byte offsets and any declared alignment gaps. In tiled mode, tiles are
ordered by tile row then tile column within each plane, with all tiles of one
plane preceding the next plane. This matches the selected R tiles, then G tiles,
then B/alpha organization. Every tile starts on a page boundary, including when
the following tile begins a new plane. Row padding, inter-tile alignment gaps
and any final reservation rounding are storage space, not logical pixel coordinates.

Let the common tile geometry be (Th,Tw), and let an image plane have extent H,W.
The logical grid is anchored at image coordinate (0,0), independent of a requested
ROI. A pixel (y,x) maps to tile (floor(y/Th),floor(x/Tw)) and local coordinate
(y mod Th,x mod Tw). Each tile's valid extent and storage offset can be derived
by the checked formulas below; an implementation need not allocate a directory
entry for every tile. One global stride vector generally does not represent
tiled-plane addressing.

For tile t with valid extent ht,wt and byte offset ot:

```
row_pitch    = Tw*d
valid_bytes  = ht*wt*d
padding_bytes = ht*(Tw-wt)*d
storage_span = ht*Tw*d
next_offset  = align_up(ot + storage_span, P)
alignment_gap = next_offset - (ot + storage_span)
```

An interior tile has ht=Th and wt=Tw and occupies a tight Th*Tw block. A right
edge pads only the end of each valid row. A bottom edge keeps its actual row
count; it does not reserve the missing Th-ht rows. Thus a 2x72 edge with T=128
occupies 2x128 sample slots, not 128x128. The next tile is still page-aligned.
P is the host page size and the image base is page-aligned. Full interior tiles
also obey this start-alignment rule: a tight payload smaller than P can be followed
by an alignment gap. The image reservation base/size obey platform granularity.

Let nx=ceil(W/Tw), q=floor(H/Th), hrem=H mod Th. Each tile row has a common valid
height. Define the following checked layout terms:

```
full_step = align_up(Th*Tw*d, P)
edge_step = 0 if hrem=0 else align_up(hrem*Tw*d, P)
plane_step = nx*(q*full_step + edge_step)
ht = min(Th, H-ty*Th)
tile_offset = plane_offset + ty*nx*full_step + tx*align_up(ht*Tw*d, P)
sample_offset(y,x) = tile_offset + (y mod Th)*Tw*d + (x mod Tw)*d
```

For equally sized planes, plane_offset=c*plane_step. The image span includes
the aligned tile slots; final platform reservation rounding can add a tail.
Tile payload occupies only ht*Tw*d bytes within each aligned slot. Page backing
is prepared for the page intervals intersecting authorized sample ranges.
Page alignment does not make the complete page's sample coverage valid.

The grid has ceil(H/Th)*ceil(W/Tw) tiles. Boundary valid extents are clipped to
H,W. No padding is a valid sample, implicit zero pixel, or filter boundary rule.
Storage padding must not affect color results, sample identity or dirty mapping.
Byte count, pitch, offset, alignment rounding and shape products use checked
arithmetic before proportional allocation.

Tile size is a graph policy rather than an operator parameter. The 128x128
planning default remains configurable. **Tile height and width must each be a
positive power of two. Non-power-of-two tile geometry is unsupported** and is
rejected with InvalidArgument by `Compiler::plan`; `ResultBuilder` validates
the tile geometry when it creates private backing, including for continuous
storage. Image and ROI extents may be arbitrary positive sizes;
incomplete edge tiles retain their actual valid extent. Validated geometry
permits shift/mask address calculation.
Halo reads and cross-tile ROIs may exceed a tile; they do not change stored output
tile geometry. Tileless numeric tensors are not assigned fictitious image axes.
Incoming image storage that does not match the required planar/tiling layout
must be normalized through the explicit import/layout conversion boundary.

## Backing, views and exact access

Contiguous means one reserved virtual byte span with a shared lifetime owner,
including its alignment gaps. It does not require adjacent physical RAM frames
or eagerly provided backing for the full image. Each coordinate has a stable
address offset within that range. Internal plane/tile access retains the backing
needed by its bounded window. The owning Result retains every frame/layer
backing and all pages already containing published data. Closing an internal
access window does not evict those pages while the Result remains alive. The
last Result owner releases its image pages, virtual reservations, and retained
input associations.

An image access request uses the Result schema and an exact logical sample set.
The coordinator maps it to page/tile portions and the typed `ResultImageInput`
exposes reads bounded by the captured descriptor, selected slot, published
coverage, and declared Need. A C++ or C callback reads samples through its
Result service; it cannot construct or retain a `PlanarImage` owner or acquire a
standalone image window. Crossing tiles does not require collecting the complete
image. Physical page/transport granularity may exceed logical demand and is
accounted separately. `ResultBuilder::publish_image` writes exact output regions
and certifies their support before publication. Packed staging may occur inside
the host, but it is not a second authoritative image representation.

Distinguish virtual reservation, provided page backing, and valid produced sample
coverage. A newly provided zero-filled page does not make all its pixels valid.
In continuous-plane storage a page can cross plane boundaries; in page-aligned
tiled storage a page does not contain the following tile, but can contain other
unrequested samples of its own tile. Requesting one component does not compute
or validate other samples sharing its pages. Missing sample coverage
is never an implicit zero. Published regions remain immutable, including when
other disjoint regions in the same image are produced later.

The structured Result coordinator explicitly prepares source backing and
upstream data before a callback reads samples, with budget and cancellation
checks. Private host windows retain pages while in use; callback capabilities
remain bounded to the Result Need. Kernel dispatch and resource failure are not
hidden in a synchronous fault handler. This does not promise that the operating
system itself never incurs an ordinary demand-page fault.

The public Result API does not expose the reserved range as an unconditionally
readable ByteView or a standalone `PlanarImage` factory. Image reads require a
valid Result descriptor and published sample coverage. Dense export requests
its complete region explicitly. A raw numeric consumer does not bypass either
requirement.

Accounting separates reserved virtual bytes from page-backed bytes and metadata.
Charge each actually provided page's full capacity, including any row/alignment
gaps sharing it, plus staging and simultaneous old/new backing. A small view
may retain more backed pages than its logical payload, so both amounts must be
reported. Page provision/commit is not a portable promise about physical RSS.
Address reservation limits and sparse bookkeeping need explicit admission; do
not allocate one metadata record for every possible page in a huge unused range.
Cancellation, source failure and allocation/provision failure fail the affected
Result publication and release unpublished resources normally.

## Selected lifetime and failure policy

Page backing is provided on demand and produced pages remain until image
retirement. An admission failure reports ResourceExhausted rather than evicting
live image data, replaying its producer or creating temporary-file backing.
No pinned physical-residency guarantee follows: accounting describes supplied
page backing, while OS residency is separate. Active access windows cannot be
revoked. Unpublished failed allocations can be released normally, without
discarding already published observations or data sharing a provided page.

The CPU Result owner and its private planar backing implement this policy.
Packed ROI reads are bounded accesses to the Result's image slot; they do not
create another semantic image object or publication route.

## Checked row-padded edge example

Use W=200, H=130, C=4, Float32, T=128 and reservation/page granularity P=16384.
The first plane has these tiles in order (all offsets/pitches are bytes):

| Valid height | Valid width | Row pitch | Tile offset |
| --- | --- | --- | --- |
| 128 | 128 | 512 | 0 |
| 128 | 72 | 512 | 65536 |
| 2 | 128 | 512 | 131072 |
| 2 | 72 | 512 | 147456 |

The next plane starts at 163840. Four planes reserve 655360 bytes (640 KiB):
416000 valid sample bytes, 116480 row-padding bytes and 122880 alignment-gap/tail
bytes. Tile payload plus row padding totals 532480 bytes (520 KiB).
R at (y=129,x=199) has byte offset 148252.
With no existing backing, preparing this one sample
requires the page at index 9, or 16384 backed bytes, while only four sample bytes
are requested. It does not make the other pixels in that page valid. This also
shows why valid-byte count, virtual span and page-backed capacity are separate.

## Result-owned image storage and public interfaces

The public contract has independent package, workflow, traits, and plugin
versions: package 0.29.0, WorkflowDocument schema 4, semantic operation traits
21, numeric C operation ABI 11, and Result operation ABI 1. These versions are
not interchangeable. Structured Results own image publication and lifetime through typed slots backed by planar pages.

```cpp
struct ResultImageSpec final {
  ResourceString key;
  std::uint64_t frames = 1, layers = 1;
  ValueDescriptor descriptor;
  PlanarImageLayout layout;
  std::vector<ValueFacet> facets;
};

class ResultRef final {
 public:
  const SchemaTemplate& schema() const;
  Result<ResultDescriptor> descriptor(bool require_complete = true) const;
  Status read_image(const ResultDescriptor&, std::uint32_t slot,
                    const std::vector<std::uint64_t>& coordinate,
                    void* destination, std::size_t bytes) const;
};

class ResultBuilder final {
 public:
  Status publish_image(std::uint32_t slot, const Region& region,
                       ByteView packed, ResultRelation relation,
                       ResultFinality finality);
  Result<ResultRef> seal();
};
```

The excerpt omits schema fields, budget/cancellation arguments, and unrelated
Result operations. `SchemaTemplate` holds image slots and any primitive fields
that share the Result contract. `ResultFieldSpec` remains packed primitive
records; image samples use typed image slots backed by planar pages.

`ResultRef` is the public owner of image schema, published sample coverage,
dependency relation and lifetime. `ResultRef::capture()` snapshots the certified descriptor, field/image relations,
descriptor basis, and dependency bundle at one revision. The Result’s ordered
source association remains separate live state and may grow monotonically as
inputs are consumed. The host actor publishes this captured view, preventing an
observer from pairing earlier evidence with a later prefix. Result resources
retain schema-declared owners, including ICC/OCIO resources referenced by
typed image facets; the compiler selects nested resources and runtime bindings
are admitted again under the execution resource root. The public `PlanarImage` class retains
structural `validate_layout` checking and read-only declarations, but creation,
import, view assembly, read/write acquisition, packed publication, and image
reads are private implementation methods accessible to `ResultBuilder` and
`ResultRef`. Valid backing is never returned as an independent application
owner. A Result contains one internal planar backing per frame/layer pair.
`ResultBuilder` creates that backing under the Result budget, and the last
owning Result reference retires its pages and virtual reservations.

The memory counters describe this internal backing: reserved virtual bytes,
page-backed bytes, charged metadata and valid samples are distinct. A window or
view may retain pages that contain samples outside its logical read set. The
resource root accounts the page capacity, metadata, staging, relation/maps,
continuation state, queued work, and retained owners under their respective
limits. `resident_bytes()` is an accounting snapshot, not measured process RSS.
An allocation, page preparation, cancellation or callback error cannot publish
success for the affected observation. Already published regions remain
immutable; overlap is rejected, and the last Result owner releases its backing.

## Compiler, execution and CPU services

WorkflowDocument schema 4 declares a structured input with
`WorkflowInputDeclaration.result_schema`; `ExecutionBinding.result` supplies its
owning `ResultRef`. A named image output is a Result output and appears in
`ExecutionResult.results` (or `DemandResult.results`). Numeric Values retain their ordinary nonzero extents and affine tensor layout.
The structural image facet is rejected by Value storage.

`PlanningOptions` captures named output regions and positive power-of-two tile
extents. The compiler resolves the selected output and image-slot schema before
execution. `ResultProgramQuery` captures the output index, image slot and
requested image footprint; tile height/width affect physical backing geometry.
Image footprints use flattened `{frame, layer, descriptor axes...}` samples and
close over all channels for the semantic Image or ColorArray tuple contract.
TDM-only facets and layout groups do not add sample support or alpha demand.

The operation registry selects `start_result` for structured outputs. A
`ResultProgramNeed` can request typed image samples from a Result input, alongside
Value fragments, structured Result fields and bounded host I/O. The coordinator
validates each Need against the source Result descriptor and published coverage.
The callback reads through `ResultImageInput`/`ResultProgramPhase::read_image`;
the CPU parallel and CPU tile services provide host computation facilities
according to the registered operation contract. The callback publishes image
regions and exact dependency evidence through `ResultBuilder`; it does not
receive a PlanarImage owner or writable PlanarImage window. C operation modules
use `result_operation_plugin_api.h` ABI 1 for the corresponding typed Result
ports and services. The C base numeric table remains ABI 11.

Result execution is coordinated by the same plan and execution context as
numeric Values. The standard execute/frozen and exact-demand paths return
named Results; `execute_stream` observes selected Result publication through its
Result observer. Dependency relations preserve selected sample support across
Data, Control, Validation and Descriptor roles. A changed, consumed Control
sample can invalidate the old relation and cause the next request to discover
new Data samples. A truly unconsumed control or unrelated tile adds no support.
Typed Result runtime descriptor observations can participate in dirty transpose
with role bit 8. Numeric Value descriptor/facet changes are static and require
recompilation; numeric Value dirty queries accept payload roles 1..7. Result
schema changes that alter the compiled contract also require recompilation.
See [Global Results](../kernel-architecture/Global-Results.md),
[Dependency Data](../kernel-architecture/Dependency-Data.md), and
[Region Semantics](../kernel-architecture/Region-Semantics.md) for the wider
Result and dirty contracts.

Production image operations that still use image Value contracts are not runnable through Result image slots. Their current source and registry status are listed in [Image operations](../kernel-architecture/Image-Operations.md).

## Current fixture sources

[`test_unified_result_images.cpp`](../../tests/integration/test_unified_result_images.cpp)
uses test-defined operations for Result image input/output, mixed numeric
control and Result ports, frame/layer samples, dynamic support and dirty
replacement. [`test_result_plugin.cpp`](../../tests/integration/test_result_plugin.cpp)
loads a C11 Result module. CMake also defines an installed-package consumer
check. All 13 focused checks passed, including 8192-row paging, prefix
publication, cancellation, shared execution, captured facts, owner retirement,
tuple closure, cross-frame support, dirty transpose, alias root-cache reuse,
numeric multi-output behavior, and bounded proof work. The C11 Result fixture
and native Metal Result fixture passed; the latter dispatched and submitted
once and read back float value 4. Native GPU checks also passed affine and
broadcast packed transfers and root-work rejection at a 500-unit limit. Five
installed consumers passed: unified workflow, C++, Result contracts, C11, and
native GPU. The C fixture verified dynamic Field-domain binding after consuming
`ResultObjectNeed`, including Descriptor/Field replacement between zero and
nonzero rows. Insufficient `source_support()` budget returns typed
`ResourceExhausted`.

## Platform reference boundary

[Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
distinguishes reserve and commit, including host page/allocation granularity and
the distinction between commitment and physical allocation. It supports the
terminology here. The CPU storage implementation uses platform-specific virtual
reservation and page-provision mechanisms; these notes make no claim that every
target operating system has passed runtime validation. The CPU address formulas
do not define native GPU image mapping; an accelerator path must retain the
Result slot's ownership, sample authorization and publication contract.
