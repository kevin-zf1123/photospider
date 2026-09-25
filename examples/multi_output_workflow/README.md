# Multi-output image migration example

This C++17 source uses public named-output and optional joint-execution APIs.
Package 0.20.0 removes the `420` scenario and its `color.rgb_to_ycbcr420` dependency.
Three legacy image scenarios remain for migration: `split`, `channels` and
`gaussian`. They build, but the legacy typed image bindings are rejected by the
current planar storage gate. They are not active runtime acceptance tests.

```sh
cmake --build build --target photospider_multi_output_workflow -j 8
build/examples/multi_output_workflow/photospider_multi_output_workflow --help
```

The source retains independent offset, convolution and Gaussian coefficient
oracles. Gaussian recomputation takes a separately supplied R reference plane;
it no longer calls the retired channel-extraction operation. The selectors
`all|split|channels|gaussian`, `--joint on|off`, `--radius` and `--sigma` remain
available for migration work, but successful image execution requires migration.

Standalone configuration consumes installed Photospider 0.24:
`cmake -S examples/multi_output_workflow -B build/multi-output-consumer -DCMAKE_PREFIX_PATH=/path/to/install`.
This optional migration source is not built or executed by the installed-consumer
CTest target. Current multi-output correctness is covered by
`test_multi_output_execution` and the planar workflow tests.

See [multi-output contracts and support boundary](../../docs/kernel-architecture/Multi-Output-Operations.md)
and the [FMT retirement record](../../docs/built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md).
