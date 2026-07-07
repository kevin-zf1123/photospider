# ImageBuffer Memory Contract

`ImageBuffer` is a public kernel contract. Operators, schedulers, plugins,
adapters, cache code, and debugging tools may depend on the fields and
invariants in this document.

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

`InputTile` is a read-only non-owning view into an upstream `ImageBuffer` plus a
`cv::Rect` ROI. It carries a `const ImageBuffer*` so tiled operators cannot
replace or mutate upstream buffer metadata through the tile API. `OutputTile` is
the writable counterpart for destination regions and carries a mutable
`ImageBuffer*`. `TileTask` combines one `OutputTile` with zero or more
`InputTile` views for a tiled operator callback.

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

## ARM Mac Alignment

64-byte row alignment is the portable minimum. ARM Mac high-performance paths
may need or benefit from 128-byte alignment. That is an optimization target and
should be benchmarked before becoming a stricter default.

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

The Metal buffer adapter is implemented in `include/adapter/buffer_adapter_metal.hpp`
and `src/adapter/buffer_adapter_metal.mm`, but it is not currently enabled in
the core library build and is not connected to the production compute path.
`CMakeLists.txt` builds the current Metal operation path separately through
`src/metal/perlin_noise_metal.mm` and `custom_ops/metal_ops_loader.cpp`; that op
path registers `image_generator:perlin_noise_metal` when the loader directory is
manually present in `plugin_dirs`.

Implemented adapter behavior:

- Upload supports `FLOAT32` buffers with 1 or 4 channels.
- Uploaded buffers use `Device::GPU_METAL`.
- `context` owns a Metal texture holder.
- Download returns a new CPU `ImageBuffer`.

Plugin, scheduler, and core compute code must not treat the Metal buffer adapter
as a production runtime boundary until a future change wires it into the build,
documents the call chain, and records validation evidence. Direct interpretation
of `context` remains backend-specific and should not become a broader contract
without benchmark evidence.

## Benchmark Questions

Before tightening Metal access policy, benchmark:

- Adapter API vs direct context access for 16x16 and 64x64 tiles.
- Lightweight operations such as copy, add, and simple curve transforms.
- Upload/download-heavy workloads.
- 64-byte vs 128-byte row alignment on ARM Mac.
