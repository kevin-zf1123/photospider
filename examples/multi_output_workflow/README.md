# Multi-output split example

This C++17 source uses the public named-output and optional joint-execution APIs.
The maintained scenario requests the independent `full`, `left`, and `right`
outputs of `image.split_horizontal` and checks their source offsets. The old
filter scenarios have been removed with their built-in implementations.

```sh
cmake --build build --target photospider_multi_output_workflow -j 8
build/examples/multi_output_workflow/photospider_multi_output_workflow --joint on
```

The typed image binding still needs planar migration before this example can be
used as runtime acceptance. The active named-output and planar tests are
`test_multi_output_execution` and the planar workflow tests.

An installed package can build the source with
`cmake -S examples/multi_output_workflow -B build/multi-output-consumer -DCMAKE_PREFIX_PATH=/path/to/install`.
See the [multi-output contracts](../../docs/kernel-architecture/Multi-Output-Operations.md).
