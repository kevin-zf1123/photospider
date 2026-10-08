# Metadata assignment execution example

This executable exercises the public Result workflow for metadata assignment.

It builds a Float32 tensor, binds it as a Result input, compiles a requested output region, executes the edit, and checks the resulting metadata and selected sample bits.

It is a runnable correctness and accounting example; it does not publish a performance comparison.

```sh
cmake --build build/kernel-dev --target photospider_metadata_performance -j 8
./build/kernel-dev/examples/metadata_performance/photospider_metadata_performance \
  131 tiled view patch roi 1 4 strict
```

The positional arguments are `size`, `storage`, `layout`, `edit`, `request`, `repeat`, `channels`, and `profile`. Defaults are `512 tiled auto patch full 7 4 strict`. The CLI accepts sizes from 130 through 4096 and channel counts from 4 through 32. `storage` selects `generic`, `tiled`, or `continuous` source construction; `layout` selects `auto`, `view`, or `materialize`; `edit` selects `patch`, `replace`, or `cascade`; and `request` selects `full`, `one`, or `roi`. The ROI is rows `[127,130)`, columns `[126,131)`, and channel 2. For size 131 this requests 15 Float32 samples, or 60 logical bytes, including all four corners checked by the example.

```sh
./build/kernel-dev/examples/metadata_performance/photospider_metadata_performance \
  131 tiled view patch roi 1 4 strict
./build/kernel-dev/examples/metadata_performance/photospider_metadata_performance \
  131 generic materialize replace roi 1 4 strict
./build/kernel-dev/examples/metadata_performance/photospider_metadata_performance \
  131 continuous view cascade roi 1 4 strict
```
