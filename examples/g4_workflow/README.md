# G4 dependency workflows

This executable demonstrates dependency-aware workflows through the public Result API. Its inputs and operation outputs are Results. The `data` case uses a billion-element logical tensor with only two published samples, while the other cases exercise progressive Needs, shared work, ordered reductions and scans, and block-state reuse. The small `Value` used by the generic `InputSnapshotStore` check is local typed backing for that separate API demonstration; it is not a workflow binding.

The executable has three entry modes:

```sh
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow --radius-only
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow --scenario data
```

With no arguments it runs all nine cases: `data`, `progressive`, `dynamic`, `demand`, `shared`, `radius retention`, `reductions`, `scan`, and `blocks`. `--radius-only` runs `dynamic`, `demand`, and `radius retention`. `--scenario` selects one of `data`, `progressive`, `shared`, `reductions`, `scan`, or `blocks`.

## Sparse data and support

The `data` case declares a Float64 tensor with one billion logical samples and binds a Result that publishes samples at coordinates 1 and 999999998. One `execute_fragments` call requests just those two points. The example checks their values, rejects a read from the unmaterialized middle, verifies `dependencies.source_support()` equals the requested footprint, and checks that `potential_dirty()` maps a changed input sample to the matching output sample. The sparse Result stays within a 4096-byte Payload budget. These dependency relations describe logical support; they are not counts of storage reads or data transfers.

## Progressive control and data Needs

The `progressive` case registers two no-input Result source operations and a staged `Follow` operation. The control source provides values at coordinates 0, 1, and 3; each value selects the next coordinate until the final control selects payload coordinate 999999999. `Follow` requests ResultProgramNeeds with Control role 2 for the control tensor and Data role 1 for the payload, then publishes value 17.25 with both the historical control relation and the final payload relation.

The source operations generate 32 bytes in total for the requested samples. The example enforces a 128-byte Root Payload budget; the observed peak is 112 bytes. Generated-byte counters describe produced Result payload, not physical read traffic.

## Dynamic edits, demand, and retained Results

The `dynamic` case runs `numeric.radius_scatter` and `numeric.radius_gather` over bound Results. Editing `radius[3]` changes scatter output 0 from 1 to 5 while gather output 0 remains 1. A frozen execution still returns scatter value 1. The dependency evidence marks scatter output 0 dirty for the radius edit, leaves gather output 0 clean, and reports the newly selected data edge only in the updated generation. See the [dependency sampling contract](../../docs/kernel-architecture/Dependency-Sampling.md) for the operator behavior.

The `demand` case opens a context-owned handle and requests the sparse footprint `{0,4}`. It changes the radius and then the data binding without recomputing between edits, so dirty coverage accumulates across both replacements. The latest result is `[10,14]`; a frozen bundle continues to return `[1,5]`. A read from the middle hole is rejected, and the example releases the exact query subscription.

The `radius retention` case requests the same frozen query twice while retaining the first Result. The warm request returns the same Result ObjectId and has no operation timings. A new binding generation recomputes even when a data edit is outside the query and leaves dirty coverage empty. A subsequent radius edit changes the dirty footprint. After clearing the result cache, the retained output still reports its exact selected data support. This case demonstrates frozen Result retention and dependency evidence; it does not claim cross-generation content-cache reuse for radius operations.

## Shared execution

The `shared` case sends two exact waiters to the same immutable Result bundle. A barrier inside a registered identity operation lets the second caller join the in-flight computation before the first caller is cancelled. The first request returns `Cancelled`; the second still receives value 7 and complete dependency evidence. The callback runs once, including when Result retention is disabled.

## Ordered reductions

The `reductions` case connects a no-input Result source operation to `numeric.mean` and `numeric.variance`. Each source call generates 64 Float64 samples. Across 192 generated blocks, the source produces 98,304 bytes while the Root Payload peak is 624 bytes under a 1024-byte cap. For the repeating values `[0,1,2,3]`, the independently checked outputs are mean 1.5 and population variance 1.25. Both outputs record support for the full 4096-sample input. The byte count is generated output payload, not a measurement of physical reads. The scalar reductions use ordered incoming accumulators rather than partial block sums.

## Ordered scan

The `scan` case computes all 128 inclusive prefixes of `[1,2,...,128]` with block size 16. A Result source operation generates monotonically from each requested span; the example verifies each of the 128 samples is generated once and checks the triangular-number result ending at 8256.

A second binding contains `[1,+inf,...]`. A request for `{0}` succeeds with 1, while the joint request `{0,1}` reports the non-finite value at input 1. This demonstrates request-local validation at the selected prefix boundary.

## Block-state reuse

The `blocks` case runs `numeric.ordered_scan` over `[0,1,2^54,4,5,6]`, then changes the first bound sample to 1. The initial execution has six block-cache misses. After the edit, the first three transitions miss and the last three hit because their incoming accumulator state has reconverged. The prefix through input 1 is 2, and the final output is checked with a volatile binary64 left fold. Dependency support still covers the complete current source prefix. A cache hit reuses a block result, but the operation still supplies the Result Need used to establish the dependency.

## Build and run

Build the in-tree executable and run either the complete demonstration or a selected case:

```sh
cmake --build build/kernel-dev --target photospider_dependency_workflow -j 8
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow --scenario progressive
ctest --test-dir build/kernel-dev -R '^test_dependency_(workflow|radius_workflow)$' --output-on-failure
```

The full workflow test invokes the no-argument mode. The radius test invokes `--radius-only`.

The same executable can be built against an installed Photospider 0.32 package. The consumer test project also provides `photospider_sampling_consumer`; the dependency workflow tests cover the no-argument and radius-only modes.

```sh
cmake --install build/kernel-dev --prefix "$PWD/build/kernel-dev/consumer-install"
cmake -S tests/consumer -B build/kernel-dev/consumer-build \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build \
  --target photospider_dependency_workflow photospider_sampling_consumer -j 8
ctest --test-dir build/kernel-dev/consumer-build \
  -R '^(installed_dependency_workflow|installed_dependency_radius_workflow)$' --output-on-failure
```

Payload peaks and limits in these examples cover Root Payload accounting. They are not process RSS and do not include all metadata or other process memory. The local `test_dependency_workflow` and `test_dependency_radius_workflow` CTests passed, and the installed consumer's `installed_dependency_workflow` and `installed_dependency_radius_workflow` CTests passed. These results cover the listed CPU workflow cases, not other platforms.
