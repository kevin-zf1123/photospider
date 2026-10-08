# Unified Result image workflow

This example registers a small test operation and runs it through the public C++ workflow API. Its three inputs and both named outputs are `ResultRef` objects. The operation chooses image samples from two Result inputs using an Int64 Control Result, publishes a sparse image Result, and publishes an independent constant `count` Result from the same node.

## Build and run

From the repository root, build and run the example in the configured test build:

```sh
cmake --build build/kernel-dev --target photospider_unified_result_workflow
build/kernel-dev/examples/unified_result_workflow/photospider_unified_result_workflow
```

Build the example and its installed-consumer executable in the already configured consumer build, then run both installed tests:

```sh
cmake --build build/kernel-dev/consumer-build \
  --target photospider_unified_result_workflow photospider_unified_result_consumer
ctest --test-dir build/kernel-dev/consumer-build \
  -R '^installed_unified_result_(workflow|cpp)$' --output-on-failure
```

A successful run prints:

```text
count=32 frame=1 layer=0 y=1 x=2 before=1012 after=6013
```

## Result inputs and outputs

The image schema `example.image` contains one Float32 `pixels` tensor. Its cell shape is `{2,4}`, its batch axes are `{2,2}`, and its full sample shape is `{2,2,2,4}` in `{N,L,Y,X}` order. The fixture assigns each sample `1000 * N + 100 * L + 10 * Y + X`; the second image source adds 5000.

The `control` input uses schema `example.control` and one Int64 tensor named `samples` with shape `{2,2,2,4}`. Its 32 values are selectors in the range 0..7 when consumed. The low bit selects image A or B. The remaining bits shift the source x coordinate by `(selector >> 1) % 4`, wrapping within the four-pixel row.

The operation declares two Result outputs. `image` uses the image schema and contains the selected samples. `count` uses schema `example.count`, with one Int64 `samples` tensor of shape `{1}`. It publishes the constant 32 and declares no input dependencies, so requesting only `count` does not read either image or Control payload.

```text
Result A ─┐
Result B ─┼─ example.gather ── image : Result
Control ──┘                 └─ count : Result
```

The image output records one Control dependency and one selected image Data dependency for each demanded output sample. The operation first requests Control tensor support with `Control | Validation` roles (`6`), validates only consumed selector values, then requests selected image samples with the Data role (`1`). Its `sample_rows` relation records this exact per-sample support. A change to a consumed selector can change the selected branch and source coordinate; a selector outside the requested coverage does not enter the read set.

## Sparse demand and binding edits

`main.cpp` executes both outputs, reads `count` with `ResultRef::read_tensor`, then requests one image sample through a `DemandHandle`. The requested full-sample coordinate is `{N=1,L=0,Y=1,X=2}`. With the initial selector 0, the sample comes from A and equals 1012. The example changes Control sample 22 to selector 3, replaces the immutable bindings, and requests the same coverage again. Selector 3 selects B and shifts X by one, so the returned sample is 6013. The example checks that dirty coverage for `image` is exactly the requested sample. The integration test separately verifies that earlier dependency evidence remains immutable and that an unrelated selector edit stays clean.

`DemandQuery` maps an output name to a `Footprint` in the output's full sample shape. `ExecutionContext::execute_fragments` evaluates only requested coverage and leaves holes unauthorized. An empty footprint for either output publishes descriptor metadata with zero tensor coverage and reads no sample payload. A count-only request remains independent of invalid or unrequested selectors. A selector outside 0..7 fails when that selector is consumed. The fixed image and Control schemas are checked during metadata specialization, before execution.

The integration test also verifies that published Results remain readable after the `ExecutionContext` retires. Current validation passes locally for the runnable example and `test_unified_result_images`; installed consumers `installed_unified_result_workflow` and `installed_unified_result_cpp` pass 2/2. These checks cover CPU Result behavior and do not claim GPU execution. The example demonstrates Result ownership, two named Result outputs, N/L coordinates, sparse demand, dynamic support, and dirty transpose. Production image operation availability is documented in [Image operations](../../docs/kernel-architecture/Image-Operations.md).
