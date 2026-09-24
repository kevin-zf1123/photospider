# Metadata assignment and removal

This public C++17 workflow creates Float32 `[1.6, signaling-NaN, -0, +Inf]`,
assigns an independent coverage-role description plus an opaque annotation,
and removes that annotation on a second edge. It verifies all four bit patterns,
unchanged source metadata, and the two output descriptions. Assigning a role
performs no sample-domain validation or arithmetic.

```sh
cmake --build build --target photospider_metadata_workflow -j 8
./build/examples/metadata_workflow/photospider_metadata_workflow
```

Expected: `1.6, signaling NaN, -0 and +Inf preserved bit-for-bit; source unchanged;
annotation removed only from cleaned output`.

See the [schema and operation contract](../../docs/kernel-architecture/Tensor-Semantic-Metadata.md)
and [measured performance](../metadata_performance/README.md).
