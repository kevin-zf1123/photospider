---
spec_schema_version: 1
id: KERNEL-tensor-storage
kind: shared_kernel_contract
status: Proposed
implementation_status: not_implemented
clarification_status: selected_storage_policy_complete
inspection_commit: d49d1840
---

# Tensor storage and region access

This is the selected storage-policy target clarified with FMT on 2026-09-22. It is not a
description of the implemented runtime. [Chinese reader version](zh/Tensor-Storage-and-Region-Access.zh.md).
It owns physical layout, tile geometry, region access and storage ownership;
[FMT-common](../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
owns the associated color/alpha interpretation. No ADR or runtime change is
created by this specification.

## Confirmed decisions

| Topic | Selected target |
| --- | --- |
| Image layout | Every image is planar. Interleaved imports require explicit layout conversion. |
| Graph tile policy | One DAG-wide tile size; operators cannot choose different output tile sizes. Examples are 64x64, 128x128 and 256x256, not an exhaustive accepted size list. |
| Plane organization | Continuous planar storage remains available; when tiled, all tiled image planes follow the DAG geometry, with no per-plane size override. |
| Backing | Reserve one full-image continuous virtual address span; provide backing by page as needed. Plane/tile access retains the shared image address-space owner, rather than unrelated image allocations. |
| Rows | Samples within a plane/tile row are contiguous; row-end padding is permitted. |
| Color and alpha | One tensor may contain a color group and an independent alpha plane, including L*,a*,b*,Alpha. Alpha is not another Lab color coordinate. |
| Incomplete edge tiles | Retain only valid rows and pad each row to the common tile width. Interior payloads remain tight full blocks. Every following tile starts on a host page boundary; gaps are separate from row padding. No added bottom rows. |
| Page preparation | Explicitly acquire/prepare required pages before running the operator, with resource/cancellation checks. Do not use page-fault handling to dispatch DAG computation. |
| Page retention | Retain produced page backing until the image's final lifetime owner retires; fail on budget exhaustion. No automatic eviction, DAG replay or temporary-file paging. |

Graph-wide geometry replaces the earlier per-plane/per-operator tile proposal.
One continuous image virtual range replaces the earlier independent block-owner
proposal. Physical page backing is provided on demand. These do not restore
Image/Layer as special semantic carrier types.
Generic tensor shape/dtype, explicit structural layout, color groups and consumed
metadata remain separate. Raw/override does not change actual storage addresses
or turn an interleaved import into a planar image by relabeling it.

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
to alpha. Operations declare the groups/components they consume; merely sharing
one backing does not add a whole-color/alpha validation or payload-read obligation.

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

Tile size is a graph policy rather than an operator parameter. The current 128x128
planning default is a candidate retained default, not a restriction to one size.
Halo reads and cross-tile ROIs may exceed a tile; they do not change stored output
tile geometry. Tileless numeric tensors are not assigned fictitious image axes.
Incoming image storage that does not match the required planar/tiling layout
must be normalized through the explicit import/layout conversion boundary.

## Backing, views and exact access

Contiguous means one reserved virtual byte span with a shared lifetime owner,
including its alignment gaps. It does not require adjacent physical RAM frames
or eagerly provided backing for the full image. Each coordinate has a stable
address offset within that range. A plane/tile view retains the address-space
owner and the backing needed by its access window. Keeping that owner alive
retains the whole virtual reservation and all page backing already containing
produced data. Retiring a small window does not evict those pages while the image
remains alive. The final lifetime owner releases the pages and virtual reservation.

An access request identifies exact logical coverage and components. Map that
coverage to corresponding tile portions; return supported read views/fragments
or explicitly materialize a packed read region. Crossing tiles does not require
collecting the complete image. Physical transport/page granularity may exceed
logical demand and must be reported/accounted separately. Partial image outputs
occupy their offsets within the reserved full-image range. Packed temporary read
windows are permitted as explicit staging, but independent ROI allocations are
not the authoritative storage of the same image.

Distinguish virtual reservation, provided page backing, and valid produced sample
coverage. A newly provided zero-filled page does not make all its pixels valid.
In continuous-plane storage a page can cross plane boundaries; in page-aligned
tiled storage a page does not contain the following tile, but can contain other
unrequested samples of its own tile. Requesting one component does not compute
or validate other samples sharing its pages. Missing sample coverage
is never an implicit zero. Published regions remain immutable, including when
other disjoint regions in the same image are produced later.

The executor explicitly prepares the necessary source/destination pages and
upstream data before operator access, with budget/cancellation checks. Operators
receive bounded access windows retaining their pages for the access lifetime;
no window may be invalidated while in use. Kernel dispatch and resource failure
must not be hidden in a synchronous fault handler. This does not promise that
the operating system itself never incurs an ordinary demand-page fault.

Do not expose the entire reserved range as an unconditionally readable ByteView.
Access needs both provided backing and authorized valid sample coverage. Dense
export acquires its complete required region explicitly. A raw numeric consumer
does not bypass either requirement.

Accounting separates reserved virtual bytes from page-backed bytes and metadata.
Charge each actually provided page's full capacity, including any row/alignment
gaps sharing it, plus staging and simultaneous old/new backing. A small view
may retain more backed pages than its logical payload, so both amounts must be
reported. Page provision/commit is not a portable promise about physical RSS.
Address reservation limits and sparse bookkeeping need explicit admission; do
not allocate one metadata record for every possible page in a huge unused range.
Cancellation, source failure and allocation/provision failure publish no success
for the affected observation and release unpublished resources normally.

## Selected lifetime and failure policy

Page backing is provided on demand and produced pages remain until image
retirement. An admission failure reports ResourceExhausted rather than evicting
live image data, replaying its producer or creating temporary-file backing.
No pinned physical-residency guarantee follows: accounting describes supplied
page backing, while OS residency is separate. Active access windows cannot be
revoked. Unpublished failed allocations can be released normally, without
discarding already published observations or data sharing a provided page.

The storage decisions are complete at this level. Concrete platform reservation/
provisioning calls, capability reporting, metadata encoding, concurrency control
and public window APIs still need implementation contracts and validation. The
FMT-01 uses auto/view/materialize under this rule. Materialize provides requested
samples within a new full-result virtual range; packed ROI reading remains an
explicit access-window operation rather than a different primary image backing.

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

A standalone integer-address enumeration in this documentation session checked
all 104000 sample starts for unique addresses and bounds, comparing a sequential
tile-prefix construction against the closed-form offsets above. It also checked
the total reservation and that one-sample page calculation. An additional full
64x64 UInt8 tile case checks that its 4096-byte payload still advances to the
next 16384-byte page boundary under this host geometry. It did not allocate
virtual image storage, run an operator or measure OS residency.

## Current implementation and migration gates

Current [Value](../kernel-architecture/Data-Model.md) carries one affine layout
and owner. [ValueFragments](../../include/photospider/data/value_fragments.hpp)
provides exact rectangles, but recognized image/color tuples still constrain
partial-channel fragments. The current
[snapshot store](../../src/lib/data/input_snapshot.cpp) allocates blocks
independently and keeps recognized tuple channels together. These are not the
single-image continuous planar target. Its current block-size setting and the
[planner's tile options](../../include/photospider/compiler/compiler.hpp) are
also distinct mechanisms needing coherent graph policy.

The migration must provide a structural image-layout description, virtual range
reservation and page backing, regional read/write windows, row-padded edge layout, metadata group
projection, explicit import conversion, and compiler/provider validation of the
DAG tile policy. Existing contracts documenting current behavior remain current
until code and focused public workflows implement the target.

Acceptance must independently verify planar addresses for continuous/tiled modes,
nondivisible dimensions, row/page padding, same-DAG geometry consistency, multiple
color/alpha groups, cross-tile exact component reads, immutable lifetime and actual
reserved virtual versus provided page versus valid sample bytes. Verify that
pages can contain mixed produced/unproduced regions without false validity,
active windows remain usable, sparse ROI avoids full-image backing provision,
and failures clean up partial provision. Verify that padding never contributes to samples
or boundary extension, and that incompatible interleaved image bindings fail
until explicitly converted. No implementation or runtime tests have run here.

## Platform reference boundary

[Microsoft VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
distinguishes reserve and commit, including host page/allocation granularity and
the distinction between commitment and physical allocation. It supports the
terminology here, not a claim that the cross-platform storage backend exists.
The inspected local host reports P=16384 through getconf PAGESIZE; the layout
must resolve host properties rather than assume 4096. POSIX/macOS, Windows and
any GPU mapping paths require their own implementation and validation evidence.
