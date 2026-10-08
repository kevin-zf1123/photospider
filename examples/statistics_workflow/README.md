# Paged integer statistics workflow

This example registers `statistics.histogram`, `statistics.parameters`, and `statistics.grade` through the public kernel API. The workflow binds immutable `pixels` and `mask` Results directly as inputs. The C++ fixture's `variant` parameter selects values or backing layout for those bindings; it is not a workflow input or source operation.

The graph shares one Histogram Result between the Parameters node and an alias output. Parameters publishes the integer count/total/valid record and the correctly rounded Float64 mean. Grade validates that complete Result before publishing Float64 raster rows as stable prefixes. A downstream Result sink requests those prefixes and checks every graded sample.

## Build and run

In a configured Photospider build:

```sh
cmake --build build/kernel-dev --target photospider_statistics_workflow test_statistics test_statistics_callback -j 8
build/kernel-dev/examples/statistics_workflow/photospider_statistics_workflow
build/kernel-dev/examples/statistics_workflow/photospider_statistics_workflow --large
build/kernel-dev/examples/statistics_workflow/photospider_statistics_workflow --stage-admission
```

The standalone example uses the installed public package:

```sh
cmake --install build/kernel-dev --prefix "$PWD/out/statistics-install"
cmake -S examples/statistics_workflow -B out/statistics-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/statistics-install"
cmake --build out/statistics-consumer -j 8
out/statistics-consumer/photospider_statistics_workflow
```

After configuring with `BUILD_TESTING=ON`, run the focused unit and example checks with:

```sh
ctest --test-dir build/kernel-dev \
  -R '^(test_statistics|test_statistics_callback|example_statistics_workflow)$' --output-on-failure
```

The default run checks small sample counts with 64-, 256-, and 4096-byte Result windows, a 4,096-sample case with a 24-byte window, and a 65,537-sample case. `--large` runs 40,000 samples with 65,536 bins and a 24-byte Result window. `--stage-admission` exercises the Histogram factory's pre-binding stage lower-bound check. Before the smaller cases, the executable also runs a 1,000-sample, 65,536-bin profile with a 5,000-stage cap. `--stage-regression` stops after that profile.

Fixture setup creates the full caller-owned pixel and mask backings with `BufferAllocator`; `root.reference()` charges those bytes to Referenced. Host and Payload peaks exclude these input bytes. The Root Referenced cap is `18*n` bytes for `n=H*W`. Each run reports `referenced_peak` and checks that live Referenced and Disk usage return to zero after its Results and loaded windows are released. The independent map and numeric reference state are caller-owned outside Root accounting. Each run configures explicit Root limits. The nominal cases use 1 MiB for Host and Metadata, 10 million `ExecutionOptions::maximum_dependency_work` units per Run, a 100,000-stage limit per continuation, and 32 KiB Payload; dedicated failure cases use smaller limits. The case that retains Results from two separate Runs uses 2 MiB for Host and Metadata. The 65,537-sample case uses 100 million Run work units. `--large` configures 4 MiB Host/Metadata, 1 GiB Disk, 1 billion Run work units, a one-million-stage limit per continuation, and 32 KiB Payload. `maximum_dependency_work` is a Run limit, not a Root-wide work limit. Printed `issued_stages` is the Root's aggregate coordinator count, not the per-continuation stage limit. Typed Result observations and relations add metadata as requested coverage grows, so these configured caps are not a fixed per-image metadata cost.

## Result inputs and outputs

The workflow declares `pixels` as a facet-free Int64 Result tensor of shape `{H,W}` and `mask` as a facet-free UInt8 Result tensor of the same shape, both without fields. The mask selects a sample when its byte is nonzero. Selected values must be in `[0,bins)`; masked-out values are ignored. The default fixture binding generates `x[i]=(3*i+1)%8` and selects every sample. Other fixture parameters create negative-stride pixel/mask backing or a constant zero-stride mask while preserving the same schemas.

The statistics operations accept and publish Results throughout. Histogram consumes the Int64 and UInt8 tensor Results and emits sparse `bin` and `count` fields. Parameters consumes the complete Histogram Result and emits `count_total_valid` plus `mean`. Grade consumes the Int64 tensor and the matching Parameters Result, then emits Float64 `pixels` in HW order. Its required Float64 `target` is 2. The example sink returns one Float64 tensor Result containing the checked scalar count.

Dependency support is expressed with typed Tensor, Field, and Descriptor relations. Histogram and Parameters use Conservative global support. Each grade sample depends on its source sample and on the shared parameter fields; descriptor support also invalidates the result when the input description changes. Duplicate Histogram nodes in the graph share one published Result even when the optional dependency cache is disabled.

Result associations store ObjectIds and do not own the associated source payload. Parameters copies the Histogram's numeric summary into its own fields, so releasing the Histogram Result can retire that source backing while the Parameters Result remains. A loaded field `CpuStorage` retains its read plan and Result implementation. Its bytes remain readable after the Result wrapper and execution context are gone; the loaded windows keep their field storage and Disk charge live until the final window is released.

## Bounds and reference checks

The `bin_ranges_512` recipe reserves `min(bins,512)` Int64 counters and scans the input Result once per 512-bin range. This bounds counter Payload at `min(bins,512)*8` bytes while trading memory for repeated tensor reads. Histogram tensor Needs are row-bounded, and field I/O is at most 4096 bytes per field, independent of the selected Result I/O window. Other tensor reads and field I/O are bounded by `min(page_bytes,4096)`. The 24-byte `count_total_valid` record is indivisible, so a smaller selected window returns `ResourceExhausted`.

The Histogram factory rejects a profile before source binding when its minimum source request polls leave no final poll for consumption and publication. For `H=2048`, `W=2048`, and 65,536 bins, the lower bound is `2048*ceil(2048/512)*ceil(65536/512)=1,048,576` source polls, above its one-million-stage cap. Passing this check does not reserve later output, work, I/O, or Root capacity.

The executable compares sparse bins against an independent map and checks integer totals, the correctly rounded mean, and every graded sample against a higher-precision reference. Its cases also cover empty and partial masks, invalid selected values, ignored invalid masked-out values, negative-stride Result backing, a zero-stride mask Result, a changed input binding while retaining prior Results, undersized windows, work/capacity/Disk exhaustion, cancellation after Histogram publication, and final owner release. The example reports aggregate `issued_stages`, Host, Payload, and Referenced peaks; Root counters are not process RSS or a fixed metadata cost. Result observations and typed relations make metadata use depend on requested coverage. Statistics operations use CPU stages. No CertifiedBound is claimed for Float64 grading; agreement with the example reference is a fixture check.

For the scalar domain and exact mean algorithm, see [Integer Statistics](../../docs/kernel-architecture/Integer-Statistics.md). The focused test targets are `test_statistics` and `example_statistics_workflow`.

Default cases passed locally and through the installed 0.30 consumer. The local `--large` run also passed with 40,000 populated bins, count 40,000 and total 1,309,905,304; the installed consumer was checked on the default run only. The local large run measured Root Host peak 273,268 bytes, Payload peak 6,224 bytes, Referenced peak 360,000 bytes, and 119,196 aggregate `issued_stages`. These managed Root counters are not process RSS or a fixed metadata cost.
