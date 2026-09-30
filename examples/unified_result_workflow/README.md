# Unified Result image workflow

This example registers one small test operation and executes it through the public C++ workflow API. The operation receives two structured image Results and one numeric Control Value. It publishes a typed image Result and an independent numeric `count` output from the same node.

## Build and run

From the repository root, build and run the example with the configured test build:

```sh
cmake --build build/kernel-dev --target photospider_unified_result_workflow
build/kernel-dev/examples/unified_result_workflow/photospider_unified_result_workflow
```

The example source and its test-defined operation are in this directory. The workflow uses the installed public headers and kernel target when built as an external consumer; it does not load any production image operation.

A successful run prints:

```text
count=32 frame=1 layer=0 y=1 x=2 before=1012 after=6013
```

## Data flow

The input schema `example.image` has one `pixels` image slot with two frames and two layers. Each frame/layer has a Float32 `{height=2, width=4}` sample domain. The source fixture assigns each sample the value `1000 * frame + 100 * layer + 10 * y + x`; the second source adds 5000.

```text
Result A ─┐
Result B ─┼─ example.gather ── image : Result
Control ──┘                 └─ count : Int64 Value
```

The `image` output reads only samples selected by Control. Its low bit selects A or B; the remaining bits shift the source x coordinate. The operation records the Control and selected image samples in the Result relation, so later edits transpose through the same dependency evidence. An unused Control sample stays outside the read set.

The public workflow declares both named outputs. The full execution result stores `image` as a `ResultRef` under the workflow output name and stores `count` as a numeric Value. The image can be read with a captured `ResultDescriptor` and `ResultRef::read_image`; its frame and layer are explicit coordinates.

`main.cpp` runs the complete example: it compiles the graph, executes both outputs, opens a demand handle for one image sample, reads the sample, changes the selector, replaces the bindings, and requests the same coverage again. It checks the returned values and the dirty Region before printing.

## Sparse requests and edits

A `DemandQuery` maps an output name to a `Footprint` in that output's logical shape. `ExecutionContext::execute_fragments` evaluates only that requested coverage and preserves holes as unauthorized. `DemandHandle::replace_bindings` compares consumed source observations for a new immutable binding generation; changing a consumed Control can select new source samples, while changing an unconsumed Control sample stays clean.

The example is deliberately small. It demonstrates Result image ownership, mixed outputs, named selection, N/L coordinates, sparse demand, dynamic support, and dirty transpose. Current production image availability is documented in [Image operations](../../docs/kernel-architecture/Image-Operations.md).
