# Image operations

The default registry contains image and mask keys with legacy `Value` callbacks. Their registration does not mean that a workflow can bind a packed image or pass a structural `PlanarImageLayout` to them. Input validation rejects a `photospider.image` facet on a declaration without planar layout with `InvalidArgument` (`image declaration requires planar layout`). The compiler also rejects a structural planar image when an operation's `OperationTraits::planar_storage_capable` flag is false; the legacy image callbacks below do not set that flag. Operation plugin C ABI 11 and planar extension ABI 3 describe separate interfaces and do not change these checks.

## Registered image Value operations

| Key | Legacy Value inputs and result | Region behavior |
| --- | --- | --- |
| `image.exposure_gain`, `image.opacity` | `RgbaFloat32` image and Float32 scalar (`[0,16]` for gain, `[0,1]` for opacity); output preserves the image schema | Elementwise |
| `image.mask` | `RgbaFloat32` image and Float32 mask; image output | Elementwise |
| `image.source_over` | Foreground and background `RgbaFloat32` images; image output | Elementwise |
| `image.mix` | Two `RgbaFloat32` images and Float32 mask; output preserves first image schema | Elementwise |
| `image.downsample_box`, `mask.downsample_box` | Image or mask and required Int64 `factor` in `[1,16]`; reduced spatial output | Shrink |
| `image.brush_circle` | Image and ordered Float32 scalar inputs for center, radius, color and alpha | Elementwise |
| `image.stmap` | Image and a coordinate-map Value | Dependency |
| `image.split_horizontal` | Typed Float32 rank-3 image and required Int64 `split_x`; ports `full`, `left`, `right` | Dependency, with optional joint execution |
| `image.local_inpaint_navier_stokes_native_apple_silicon`; optional `image.local_inpaint_navier_stokes_openCV` | Image and mask plus operation-specific settings | Whole |

These registered callbacks process `Value` storage using descriptors, facets, strides and regions admitted by their port schemas. They do not implement planar page access. The image callbacks have `planar_storage_capable=false`, including `image.exposure_gain`, `image.opacity`, `image.mix` and `image.split_horizontal`. A workflow that declares a packed `photospider.image` input fails input validation with `InvalidArgument`; a structural planar declaration reaches the operation capability check and fails compilation with `TypeMismatch`.

For legacy RGBA Values, `image.exposure_gain` multiplies RGB by a Float32 gain in `[0,16]` and copies alpha; `image.opacity` multiplies all four channels by a Float32 opacity in `[0,1]`. `image.mask` multiplies all four foreground channels by the matching Float32 coverage sample. `image.source_over` computes `F + B * (1 - F.alpha)` for premultiplied foreground and background values. `image.mix` computes `(1-M)A+MB` for all four channels and preserves the first image's semantic facet. The box-downsample operations require an Int64 factor in `[1,16]`; output dimensions are input dimensions divided by the factor and rounded up. `image.brush_circle` receives scalars in x, y, radius, red, green, blue, alpha order. Its center and RGB values are finite Float32, radius is a positive normal Float32, and alpha is in `[0,1]`. `image.split_horizontal` requires `0 < split_x < W`; its `full`, `left`, and `right` outputs map `(y,x,c)` to the source `(y,x,c)`, `(y,x,c)`, and `(y,x+split_x,c)` respectively. Each port has its own output coordinates and dependency request. Joint execution can share transport for requested members; it does not grant planar storage access.

The independently built [`rgba32f` C module](../../plugins/ops/rgba32f) provides its own legacy operation callbacks and optional Metal shaders. It is loaded as trusted native code through operation ABI 11. The package does not export the planar operation extension, and its callbacks do not gain planar access by being written in C or dispatched to Metal. GPU execution requires a matching module, selected backend and available device.

## Planar image execution boundary

`PlanarImageLayout` represents channel planes with separate physical storage while preserving the logical tensor shape. The default image-operation callbacks listed above do not declare the capability required by the compiler to consume that structural layout. Current public planar data, region and allocation behavior is documented in [Tensor Storage and Region Access](../kernel-specs/Tensor-Storage-and-Region-Access.md). `test_planar_image_workflow` checks the internal planar copy path; it does not establish planar support for these built-in image keys.

Use each operation's legacy Value path only when the workflow supplies an input representation admitted by its port schema. To run image work over public planar storage, compose operations that explicitly declare planar capability and document a planar callback. Registry presence, shared `image.*` naming, semantic facets and the existence of a Metal implementation do not substitute for that declaration.

## Checks and examples

[`examples/multi_output_workflow`](../../examples/multi_output_workflow/README.md) builds, but running its split fixture fails when its packed image facet is added to a workflow input declaration. The README records that the example is not current runtime acceptance. [`examples/s3_image_workflow`](../../examples/s3_image_workflow/main.cpp) is another source example; no focused CTest target is registered for it. `test_basic_operations` does not exercise `image.mix`.

```sh
cmake --build build --target photospider_multi_output_workflow test_multi_output_execution -j 8
ctest --test-dir build -R '^test_multi_output_execution$' --output-on-failure
```

`test_multi_output_execution` validates named-output host infrastructure with test-defined operations. It does not validate `image.split_horizontal` on planar input. Optional OpenCV and native GPU paths require their own build and backend evidence.
