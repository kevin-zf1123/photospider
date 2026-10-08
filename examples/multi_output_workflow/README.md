# Multi-output Result split example

This C++17 example binds one spatial `Result` and requests three named outputs from `image.split_horizontal`. The input uses schema `photospider.image`, a Float32 RGB tensor with cell shape `{3, 5, 3}` and batch axes `{1, 1}`. The operation splits at `split_x = 2`: `left` covers the first two source columns, `right` covers the remaining three, and `full` preserves the five-column image.

Each output demand requests one pixel, including all three RGB channels:

| Output | Output pixel `(y, x)` | Source pixel `(y, x)` | Output cell shape |
|---|---:|---:|---:|
| `full` | `(0, 3)` | `(0, 3)` | `{3, 5, 3}` |
| `left` | `(1, 0)` | `(1, 0)` | `{3, 2, 3}` |
| `right` | `(2, 1)` | `(2, 3)` | `{3, 3, 3}` |

The workflow runs two plans: one requests all three named outputs and one requests only `right`. It checks exact values with an independent formula, verifies each result's shape and one-pixel coverage, and confirms that reads outside published coverage fail. The right-only run also checks that sibling outputs are pruned.

The source is a `Result` created directly from the schema and bound through `ExecutionBinding::result`. The example checks ordered source association and dependency support: tensor observations carry Data and Validation roles (`5`), descriptor observations carry role `8`, and a source change at `(2, 3, channel 1)` marks only `(2, 1, channel 1)` in `right`. These observations describe dependency support, not physical read or transfer counts. Each `ExecutionResult` is inspected after its `ExecutionContext` has been destroyed, exercising the returned Results and dependency evidence ownership.

The `--joint on|off` option sets `ExecutionOptions::enable_joint`; output reports the actual `joint_groups` diagnostic. Result execution does not guarantee that joint groups will form. The example makes no zero-copy or GPU claim.

Build and run from the repository root:

```sh
cmake --build build/kernel-dev --target photospider_multi_output_workflow -j 8
build/kernel-dev/examples/multi_output_workflow/photospider_multi_output_workflow --joint on
```

To configure and build against an installed Photospider 0.33 kernel package:

```sh
cmake -S examples/multi_output_workflow -B build/multi-output-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/multi-output-consumer --target photospider_multi_output_workflow -j 8
build/multi-output-consumer/photospider_multi_output_workflow --joint off
```

The standalone project uses `find_package(Photospider 0.33 CONFIG REQUIRED COMPONENTS kernel)`. The focused multi-output behavior tests are `test_multi_output_execution` and `test_multi_output_contract`; the top-level build also registers `example_multi_output_on` and `example_multi_output_off`.
