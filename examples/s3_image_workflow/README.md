# S3 Result image workflow

This example models an editable image application on the public Result and execution APIs. It uses the built-in image operations, keeps source bindings and execution results under the same `ResourceBudget` root, and contains no dependency on the retired planar image module.

## Build and run

From the repository root, build the example in an existing configured build:

```sh
cmake --build build/kernel-dev --target photospider_s3_image_workflow
build/kernel-dev/examples/s3_image_workflow/photospider_s3_image_workflow --scenario preview
build/kernel-dev/examples/s3_image_workflow/photospider_s3_image_workflow --scenario cache
```

`--scenario` accepts `preview` or `cache` and defaults to `preview`. `--help` prints the supported options. The optional `--module PATH` loads a trusted native Result operation module through the Result C ABI into a fresh operation registry; it is not the legacy RGBA image DSO interface.

Run the repository scenarios with:

```sh
ctest --test-dir build/kernel-dev -R '^example_s3_(preview|cache)$' --output-on-failure
```

The repository tests are `example_s3_preview` and `example_s3_cache`; both pass. The installed consumer tests `installed_s3_preview` and `installed_s3_cache` also pass (2/2). Build that consumer against an installed package with:

```sh
cmake -S tests/consumer -B build/s3-consumer -DCMAKE_PREFIX_PATH=<install-prefix>
cmake --build build/s3-consumer --target photospider_s3_image_workflow
ctest --test-dir build/s3-consumer -R '^installed_s3_(preview|cache)$' --output-on-failure
```

## Image and graph

The scene contains Float32 RGBA `photospider.image` Results with cell shape `{13,17,4}` and batch extents `{1,1}` for frame and layer, a coverage Result with cell shape `{13,17}`, and unbatched Float32 `{1}` gain controls. Its full-resolution image graph runs `image.gaussian_blur` → `image.exposure_gain` → `image.mask` → `image.source_over`; Gaussian uses radius 2 and sigma 1.0. The factor-4 proxy graph is used for preview and uses radius 1 and sigma 0.25. The source image, mask and controls are Results, and the workflow publishes an image Result.

The application owns brush-event and slider policy outside the kernel. Accepted brush stamps enter a FIFO queue with capacity eight; a full queue applies backpressure. Pending slider values coalesce to the latest value. Each `tick()` applies at most one edit and one image step, and it alternates preview work with export work while both are active.

The kernel plans use a physical tile geometry of 4-by-4. Separately, a brush stamp rounds its bounding ROI outward to the 4-by-4 grid and executes `image.brush_circle` for that ROI, which can span several physical tiles, then assembles an immutable edited Result. The assembly requests 1-row windows with at most four columns and publishes zero-copy identity row views when the source permits them; these views retain the source Result owners. It falls back to ordered Planar view publication only when a requested view returns `ViewUnavailable`; this path also retains source owners without copying their samples. Unchanged rows retain their existing owners. The export retains its frozen frame while later stamps update the editable scene.

The preview first uses a factor-4 proxy graph, then publishes a full-quality frame. Publication checks content version, viewer target, and quality so stale results, foreign targets, and quality downgrades are rejected. `TileRun` is application code that issues twenty separate 4-by-4 Result region requests for the export. That application query count is distinct from the plan’s physical 4-by-4 tile geometry. It does not demonstrate the kernel splitting one atomic full-domain request or reproducing the former Value tile stream.

## Scenarios and expected checks

The `preview` scenario starts an export, fills the brush queue, observes backpressure, advances the application, retries the rejected stamp, and coalesces two slider events. It compares brush results bitwise against an ordered stamp oracle and checks the final preview and export against independent image oracles. Its output reports nine applied stamps, twenty export regions, twenty-seven preview regions, and three rejected publications or admissions.

The `cache` scenario warms the graph, changes gain, edits one brush region, changes an unrelated graph branch, and clears the result cache. A warm unchanged request has no callbacks; changing gain reuses Gaussian results; a local edit recomputes only part of the blur work; an unrelated edit preserves cached results; clearing the cache forces a rebuild. The current run reports twelve blur polls for the local patch.

The context sets a 16 MiB live Payload limit, an 8 MiB Result cache limit, and 262144 dependency-cache metadata units. This explicit allowance is part of the example configuration: the default 65536 metadata units may evict entries before the warm-cache checks. These are explicit Root/cache limits for the fixture, not process RSS limits or performance claims.
