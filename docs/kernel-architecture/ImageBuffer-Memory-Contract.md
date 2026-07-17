# ImageBuffer Memory Contract

`ImageBuffer` is a public operation and Host value contract. Operations,
plugins, adapters, cache code, Host implementations, and debugging tools may
depend on the fields and invariants in this document. Scheduler contracts do
not inspect image payloads.

## Structure

| Field | Meaning |
| --- | --- |
| `width` | Image width in pixels. |
| `height` | Image height in pixels. |
| `channels` | Number of channels per pixel. |
| `type` | Channel data type. |
| `device` | Where the authoritative data lives. |
| `step` | Row stride in bytes. |
| `data` | CPU-accessible data owner or view. |
| `context` | Backend-specific resource owner or handle. |

The public tile contract uses `InputTileView` and `OutputTileView`. Both carry a
borrowed `const ImageBuffer*` plus a backend-neutral `PixelRect`. The input view
is read-only. The output view permits adapters to expose writable pixels from
the retained payload, but the callback cannot replace descriptor dimensions,
device identity, payload ownership, or backend context. Public
`TiledOperation` callbacks receive the output as `const OutputTileView&` and
receive inputs as `OperationTileInputView` values.

The private compute layer separately uses `InputTile`, `OutputTile`, and
`TileTask`. Those backend-only values also carry `PixelRect`; private
`OutputTile` uses a mutable `ImageBuffer*` while bridging task-owned output
storage to an adapter. An OpenCV adapter may translate the rectangle only
locally when creating a matrix view; it does not retain or return `cv::Rect`
through these values. They do not cross the public operation or Host contract.

## CPU Buffer Contract

For CPU buffers owned by the kernel:

- `device == Device::CPU`.
- `data != nullptr` when image dimensions are non-zero.
- `step >= width * channels * bytes_per_channel(type)`.
- `step` may include padding.
- The base pointer must be 64-byte aligned.
- Every row start must be 64-byte aligned.

Every row start alignment means:

```text
address(row y) = data + y * step
address(row y) % 64 == 0
```

Therefore, kernel-owned allocations must pad `step` so that row starts stay
aligned even when packed row size is not a multiple of 64.

These alignment and mutability rules apply specifically to CPU buffers
allocated and owned by the kernel. `ImageBuffer::data` is a shared lifetime
handle, not a universal writable-memory promise. Producers may return a
read-only CPU snapshot and must document that boundary.

The installed IPC Host's `compute_and_get_image` result is such a snapshot. It
validates a same-user private artifact while its delivery lease protects
result-to-open, then maps the exact tight-row file with
`PROT_READ|MAP_PRIVATE`. The mapping base has the platform's page alignment,
but `step` is the packed row width, so later row starts are not promised to be
64-byte aligned. Copies of the descriptor share the mapping; the final
reference unmaps and closes its retained descriptor exactly once. Writing
through this mapping is outside the contract and may fault. A consumer that
needs writable storage or kernel-owned per-row alignment must allocate an
appropriate CPU buffer and copy rows using `step`.

## ARM Mac Alignment

64-byte row alignment is the current portable minimum. The contract makes no
128-byte guarantee on ARM Mac or any other platform.

## Stride-Aware Access

Consumers must use `step` when walking rows. They must not assume the buffer is
tightly packed.

Correct row access pattern:

```cpp
auto* row = static_cast<unsigned char*>(buffer.data.get()) + y * buffer.step;
```

Incorrect assumption:

```cpp
auto* row = base + y * width * channels * bytes_per_channel;
```

OpenCV adapters must preserve stride by constructing `cv::Mat` with the
provided `step`.

## GPU Buffer Contract

For GPU buffers:

- `device` identifies the backend, such as `Device::GPU_METAL`.
- `data` may be null.
- `context` carries the backend resource.
- Adapters define how to upload, download, and interpret backend resources.

The public contract is the `device` plus `context` relationship. The concrete
object stored in `context` is backend-specific.

## Metal Buffers

The Metal buffer adapter is implemented in
`src/lib/adapters/metal/buffer_adapter_metal.{hpp,mm}`, but it is not currently enabled in
the core library build and is not connected to the production compute path.
`CMakeLists.txt` builds the current Metal operation path separately through
`plugins/ops/metal/perlin_noise_metal.mm` and
`plugins/ops/metal/metal_ops_loader.cpp`; that op
path registers `image_generator:perlin_noise_metal` when the loader directory is
manually present in `plugin_dirs`.

Implemented adapter behavior:

- Upload supports `FLOAT32` buffers with 1 or 4 channels.
- Uploaded buffers use `Device::GPU_METAL`.
- `context` owns a Metal texture holder.
- Download returns a new CPU `ImageBuffer`.

Plugin, scheduler, and core compute code must not treat the Metal buffer adapter
as a production runtime boundary. The current production Metal operation path
owns its backend-specific objects independently. Direct interpretation of
`context` is backend-specific and is not a portable memory contract.

## Boundaries and Rationale

`ImageBuffer` is the current two-dimensional image payload and operation DSO
contract. Its channel count is not structurally limited to four, and
`FLOAT64` is a declared scalar type, but those facts do not promise end-to-end
support by every loader, operation, cache, or adapter.

This payload is not the generic graph value model. Operation results keep
named non-image values in a separate data map; neither those values nor an
opaque backend `context` turn `ImageBuffer` into an arbitrary payload carrier.
Adding a general value kind, rank/shape model, descriptor, handle, or region
requires a separate versioned design.

Current limitations are explicit:

- built-in operations may implement only selected 1/3/4-channel conversions or
  assume RGBA roles;
- some operation and image-loading paths compute in float32;
- FP4 cannot be represented because scalar size and row addressing assume an
  integral number of bytes per channel element;
- rank, N-dimensional shape/strides, quantization, named channel roles, Deep
  Image samples, and vector objects are not represented;
- `context` cannot substitute for descriptor facts needed by planning, cache
  keys, ROI, or synchronization.

Therefore 8/16-channel images and FP64 are not advertised as complete framework
contracts, and FP4, latent Tensor, Deep Image, and vector-scene values are not
supported by `ImageBuffer`. The general `Value`, descriptor, handle, and region
target is documented in the exact
[general data and regions target](../roadmap/Kernel-Evolution.md#general-data-and-regions).

The portable CPU allocation guarantee remains 64-byte row-start alignment.
128-byte alignment is not part of the current contract.

Separating immutable descriptors from writable payload views prevents parallel
tile callbacks from racing to replace ownership or device metadata. Keeping
`PixelRect` in the public view also prevents private OpenCV geometry from
becoming part of the operation ABI.

## Implementation and Validation Entry Points

- `include/photospider/core/image_buffer.hpp`
- `include/photospider/plugin/op_contract.hpp`
- `src/lib/core/image_buffer.cpp`
- `src/lib/compute/image_buffer.hpp`
- `src/lib/adapters/opencv/buffer_adapter_opencv.*`
- `src/lib/ipc/output_store.*`
- `tests/unit/test_image_buffer_contracts.cpp`
- `tests/integration/test_compute_service_split.cpp`
- `tests/integration/test_ipc_daemon.cpp`
