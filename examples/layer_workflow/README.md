# Layer and weighted-result workflow

This standalone public C++ consumer exercises RegionalSource -> compiled DAG ->
ordinary RGBA sink, plus dynamic contribution count -> canonical weighted sum ->
OptionalLayer. See [the runtime contract](../../docs/kernel-architecture/Layer-Runtime.md)
for exact operations, arithmetic, resource scope and installation commands.

```sh
cmake --build build/issue257-shared --target photospider_layer_workflow -j8
build/issue257-shared/photospider_layer_workflow
```

Independent expected output includes RGBA `[12.5,-2,1.25,1]`, midpoint count
3/5/7 numerator zero, Na=W for varying opaque weights, and an empty validity bit
for zero total weight. Assertions also check negative/insufficient-budget cases,
strict underflow, scratch cancellation, shared duplicate outputs, and backing
retirement after the last owner. No optional result cache is configured.

## Bounded raster batches

`raster.cpp` executes source → assemble → over → weight and checks every field
through the publication sink. The ordinary run also checks opacity, emission,
response, raw coverage conversion and the dense flatten sink on a 19×37 raster
with 64/256/4096-byte windows. Pixel values are signed dyadic fractions generated
from row-major position, and expected values use independent closed expressions:
for base coverage P, alpha 1/2 and emission E, self-over produces 3P/2, alpha 3/4
and 3E/2; contributions preserve these seven values and weight 1/4. Flatten on
background E produces 3P/2+7E/4 and alpha 1. All comparisons are exact for these
fixtures. The example covers tail batches, row boundaries, non-first-pixel
invalid/overflow failures, disk failure and cancellation without partial output.

```sh
cmake --build build/issue257-shared --target photospider_layer_raster_workflow -j8
build/issue257-shared/photospider_layer_raster_workflow
build/issue257-shared/photospider_layer_raster_workflow --large
```

`--large` completes the assemble → over → weight chain at 1920×1080, including
all 2,073,600 contribution rows, under a 1 MiB managed Host limit. Each raster
batch stays within one HW row, the requested field window, and 4 KiB of output
slabs. It prints actual submitted stages and the measured managed Host peak;
these are not process RSS bounds. The explicit work/stage/disk limits still
apply to larger or more expensive requests. The ordinary window-256 fixture
uses a 512-poll bound per producer, which the old per-pixel continuation exceeds.

Run the same executables through an installed package:

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-delivery/install"
cmake -S examples/layer_workflow -B out/phase-a-delivery/layer-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/layer-consumer -j8
out/phase-a-delivery/layer-consumer/photospider_layer_raster_workflow --large
```
