# File decoding and planar FP32 import

This benchmark measures the existing WebUI host codec and the kernel's public
`PlanarImage::import_value` API.

The host implementation is Sharp/libvips in
`photospider-webui/server/image-codec.ts`, also called by `Assets.importFile`.

The kernel itself does not implement JPEG/PNG/TIFF parsing.

This example does
not introduce a codec registry or change the Proposed FMT codec boundary.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target photospider_codec_performance -j 8
```

```sh
build/examples/codec_performance/photospider_codec_performance \
  build/codec-performance/run/stage_rgba8_3504x4958.tiff.f32 3504 4958 tiled 7
```
