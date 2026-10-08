# Metadata assignment and removal

This C++17 workflow demonstrates the public Result API in Photospider package
0.32.0. It creates a one-tensor Result containing Float32 values with the exact
bit patterns for `1.6`, a signaling NaN, negative zero, and positive infinity.
`metadata.assign_strict` adds a component description and an opaque annotation;
`format::remove_metadata` lowers the annotation deletion to a second
`metadata.assign_strict` node. The workflow checks both
outputs byte-for-byte and confirms that the source Result remains unchanged.

The operations edit descriptions only. They do not scan, validate, or transform
sample values. The input and output Results retain the same schema id, tensor key,
descriptor, logical axes, batch axes, and physical layout. A Result used as an
input to these operations must have exactly one tensor member and no fields.

Build and run from the repository root:

```sh
cmake --build build --target photospider_metadata_workflow -j 8
./build/examples/metadata_workflow/photospider_metadata_workflow
```

To configure it against an installed Photospider package, set
`CMAKE_PREFIX_PATH` to the install prefix:

```sh
cmake -S examples/metadata_workflow -B build/metadata-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/metadata-consumer -j 8
build/metadata-consumer/photospider_metadata_workflow
```

Expected output:

```text
1.6, signaling NaN, -0 and +Inf preserved bit-for-bit; source unchanged; annotation removed only from cleaned output
```

The older [metadata performance workload](../metadata_performance/README.md)
measures the previous Value/planar implementation. Its timings are not
performance evidence for the current Result operations.

The metadata-to-extraction configuration/resource chain
is covered; a chain through `channel.assemble` is not.

See the [metadata contract](../../docs/built-in_ops/02-format-color/op_specs/FMT-08_metadata_assignment_contract.md)
and [Result implementation guide](../../docs/kernel-architecture/Tensor-Semantic-Metadata.md).
