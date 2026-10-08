The benchmark source uses the public Result API.

## Current Result smoke

```sh
cmake --build build --target photospider_channel_performance -j 8
build/examples/channel_extraction_performance/photospider_channel_performance \
  128 continuous fp64 all 2 strict full
build/examples/channel_extraction_performance/photospider_channel_performance \
  130 tiled fp32 all 2 strict roi
```

Arguments are size, storage, dtype, layout, repetitions, CPU profile and
`full|roi`. An independent byte oracle checks the first output outside the
timed interval. These bounded runs are functional smoke checks, not a benchmark
matrix or a basis for speedup claims.
