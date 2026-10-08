# Channel extraction from a Result tensor

This public C++17 workflow uses Photospider package 0.30.0. It builds one
single-tensor Result with a B/A/R/G channel description, calls
`format::split_channels`, and publishes only handle `c2`. That handle selects the
red cell-axis component. The example requests the second row and checks the exact
UInt8 bytes `[12,13]`; diagnostics confirm that the other generated extraction
nodes did not execute.

The input has cell shape `[2,2,4]`, `channel_axis=2`, and these channel labels:

| Index | Name | Role |
| --- | --- | --- |
| 0 | B | blue |
| 1 | A | coverage |
| 2 | R | red |
| 3 | G | green |

`split_channels` returns handles named `c0` through `c3`; each refers to a
separate `channel.extract_index_strict` node's `values` Result. Only `c2` is
connected to a workflow output here. The helper checks source schema/layout and
adds assertions to generated nodes so a changed or mismatched producer fails
compilation.

Build and run from the repository root:

```sh
cmake --build build --target photospider_channel_extraction_workflow -j 8
build/examples/channel_extraction_workflow/photospider_channel_extraction_workflow
```

Expected output:

```text
Result split: red ROI = [12, 13]; only requested channel executed
```

The same example can be built against an installed Photospider package with:

```sh
cmake -S examples/channel_extraction_workflow -B build/channel-extraction-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/channel-extraction-consumer -j 8
build/channel-extraction-consumer/photospider_channel_extraction_workflow
```

The current `test_channel_extraction` integration test passed 483 arbitrary-axis
bit-copy cases, plus batch, spatial layout, fragmented backing views, metadata,
resource and limit checks. The public workflow passed on the native CPU. These
checks do not cover a subsequent channel-assembly chain, and no current Result
performance benchmark has been run. The older [channel extraction performance
workload](../channel_extraction_performance/README.md) reports Value/planar
measurements only.

See the [FMT-01 family contract](../../docs/built-in_ops/02-format-color/op_specs/FMT-01_channel_extraction_contract.md)
and [current channel operation documentation](../../docs/kernel-architecture/Channel-and-Color-Operations.md#fmt-01-channel-extraction).
