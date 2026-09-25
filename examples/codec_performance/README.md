# File decoding and planar FP32 import

This benchmark measures the existing WebUI host codec and the kernel's public
`PlanarImage::import_value` API. The host implementation is Sharp/libvips in
`photospider-webui/server/image-codec.ts`, also called by `Assets.importFile`.
The kernel itself does not implement JPEG/PNG/TIFF parsing. This example does
not introduce a codec registry or change the Proposed FMT codec boundary.

Both destination layouts use FP32: continuous separate channel planes, and
separate channel planes containing 128 x 128 physical tiles. RGB input follows
the host's existing linear sRGB + straight alpha decode; CMYK retains four
normalized ink channels and the original ICC metadata. Layout import is a
bit-preserving copy, with no additional color conversion or alpha association.
The native layout benchmark supplies a raw tensor; host ICC metadata is tested
separately by the WebUI codec tests.

Build from the kernel checkout using the existing configured build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target photospider_codec_performance test_planar_import test_planar_image_workflow -j 8
ctest --test-dir build -R '^(test_planar_import|test_planar_image_workflow)$' --output-on-failure
```

Run from the sibling WebUI checkout (its dependencies must already be installed):

```sh
npx tsx scripts/benchmark-codec.ts \
  ../photospider/build/codec-performance/run \
  ../photospider/build/examples/codec_performance/photospider_codec_performance
```

With no image arguments, the script reads the eight named fixtures from
`assets/codec/manifest.json` in the sibling kernel checkout. Optional image
arguments select exactly those files. It does not generate additional fixtures.
The corpus includes the four supplied artworks, 2048 x 2048 black PNG, white
TIFF and solid-blue JPEG, and a 129 x 131 RGBA16 PNG spanning tile boundaries.
Names follow `<content>_<channels><bits>_<width>x<height>.<format>`; the manifest
records original names and encoded dimensions. See `assets/codec/README.md`.

Artwork files are copied without re-encoding; the painting originals remain
in place. The existing local-only assets ignore policy is unchanged. Inputs
are read only during benchmarks. Outputs (`results.json`, `environment.json`
and `<input-filename>.f32` interleaved buffers) remain in the requested output
directory. Use a new directory for each comparison. Decoded FP32 buffers stay
under `build/`, separate from the encoded fixture corpus.

Each case uses two warmups and seven samples, sequentially. The first call is
reported separately; it is not a cold-storage measurement. Sharp concurrency is
one and its operation cache is disabled; the OS file cache is not flushed.
`CODEC_CONCURRENCY=4` selects four codec workers for a separate comparison;
the default benchmark setting is one, while the installed host default is four.
Kernel import is single-threaded with a fresh output owner each iteration.
The native timer includes creation, page/metadata admission, first writes,
layout conversion and publication. It excludes loading the prepared raw file,
Value construction, destruction and the full byte oracle. Decode includes
file/metadata reading, decompression, the existing color pipeline, output
allocation, and CMYK normalization. It excludes thumbnail generation, asset
persistence, IPC, daemon/workflow execution and layout conversion.

**Decode and import are separately timed stages. Adding their medians is an
estimate of their combined work, not a measured end-to-end latency.** There is
no direct native decoder-to-PlanarImage integration in the current host path;
the benchmark stages decoded bytes through a raw file outside the timers.
It therefore does not establish whole-WebUI or daemon speedups.

Percentiles use the nearest-rank definition. With seven samples p95 is the
largest observation, not a robust production tail estimate. Each native case
compares every output sample through `PlanarImageReadWindow::row_run` with its
expected interleaved source bytes, outside timing. Repeated decodes must return
identical bytes. Tests also independently check the RGB transfer function,
alpha, every 8/16-bit CMYK code, reversed and broadcast strides, unaligned input,
axis permutations, row padding, tile edges, exact special floating bit patterns,
budget failure and pre-cancellation. Running cancellation and other hardware
are outside this performance run's validation scope.

For native-only repetition using one prepared case:

```sh
build/examples/codec_performance/photospider_codec_performance \
  build/codec-performance/run/stage_rgba8_3504x4958.tiff.f32 3504 4958 tiled 7
```

Profile separately from latency measurement; filter samples to stacks containing
`PlanarImage::import_value`. Inclusive frame percentages overlap and must not be
summed. Peak process RSS is also a separate measurement from backed bytes:
`reservedBytes` is address space; `backedBytes` and `metadataBytes` are the kernel's
page and metadata accounting, not total decoder/native process memory.

See [RESULTS.md](RESULTS.md) for the measured local before/after results.
