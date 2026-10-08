`numeric.convert_format_strict` is a registered Result ABI 2 operation.

The current
performance driver builds its input Result and workflow through the public API,
compiles the graph, executes the requested coverage, and checks the first result
through a Result read window.

```sh
cmake --build build --target photospider_numeric_conversion_performance test_numeric_conversion -j 8
ctest --test-dir build -R '^test_numeric_conversion$' --output-on-failure
build/examples/numeric_conversion_performance/photospider_numeric_conversion_performance 4096 u8-f32 full 3 128
```

The positional arguments are `size`, `pair`, `coverage`, `repetitions`, and
`tile_extent`. `pair` accepts `u8-f32`, `f32-u8`, `f64-f32`, or `i64-u8`.
`coverage` accepts `full`, `channel`, or `tile`; `tile` selects a 3×3 ROI across
the tile boundary on channel 1. Tile extent is 128 or 256, and the image must be
large enough for the requested ROI.
