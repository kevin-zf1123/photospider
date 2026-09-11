# Independent outputs and Atomic joint workflows

This C++17 example uses only the installed public API. `main.cpp` contains four
small workflow constructors and independent numerical checks. It prints output
shapes, actual physical attempt counts by `node_id:output_index`, joint group
counts, and the unique Regions actually requested from each regional source.
Read sets describe transport; per-output dependency certificates remain available
in `ExecutionResult::dependencies`. Joint and singleton runs may transfer
fragments differently while computing the same values and dependency support.

## Run in the repository

```sh
cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/dev --target photospider_multi_output_workflow -j 8
build/dev/examples/multi_output_workflow/photospider_multi_output_workflow --scenario all
build/dev/examples/multi_output_workflow/photospider_multi_output_workflow --scenario all --joint off
```

Select `--scenario 420|split|channels|gaussian`; defaults are `all`, `--joint on`,
`--radius 1.25`, `--sigma 0.9`. Radius and sigma must be finite in `[0,64]`.
The example grants an explicit 100,000,000-unit dependency/work budget for the
maximum kernel and its independent recomputation. For radius greater than 8,
image and recomputed outputs request the single pixel `(1,2)`; the complete
kernel is still produced and checked. This bounds repeated upstream observations
in a demonstration with no completed-result retention. Production callers should
choose bounds for their own image size and acceptable work. Exhaustion is a
reported error, never a silently smaller kernel.

```sh
build/dev/examples/multi_output_workflow/photospider_multi_output_workflow \
  --scenario gaussian --radius 0.25 --sigma 0
build/dev/examples/multi_output_workflow/photospider_multi_output_workflow \
  --scenario gaussian --radius 64 --sigma 1.3
```

## Independent static and shared installation

Run from the repository root. Each example build finds only its corresponding
installed package and links `Photospider::kernel`; it does not include `src/`,
`plugins/`, test support, or build-tree headers.

```sh
for linkage in static shared; do
  shared=OFF
  if [ "$linkage" = shared ]; then shared=ON; fi
  cmake -S . -B "build/multi-$linkage" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS="$shared" -DBUILD_TESTING=OFF
  cmake --build "build/multi-$linkage" --target photospider -j 8
  cmake --install "build/multi-$linkage" --prefix "$PWD/build/install-$linkage"
  cmake -S examples/multi_output_workflow -B "build/example-$linkage" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$PWD/build/install-$linkage"
  cmake --build "build/example-$linkage" -j 8
  "build/example-$linkage/photospider_multi_output_workflow" --scenario all
  "build/example-$linkage/photospider_multi_output_workflow" --scenario all --joint off
done
```

The existing `test_installed_consumer` runs both joint modes too, with matching
sanitizer flags when its producer is instrumented.

## Checkable results and composition points

| Scenario | Expected result | Modify/compose |
| --- | --- | --- |
| `420` | Y 3×5; Cb/Cr 2×3; long-double BT.709 and valid-edge box oracle within 1e-7 | Connect `WorkflowNodeOutput{1,"cb"}` to a HW field operation. Y-only execution has no sibling attempt. |
| `split` | full 3×5×3, left 3×2×3, right 3×3×3; three separate pixel ROIs match source offsets exactly | Change `split_x` and `PlanningOptions::output_regions`. Right local x=1 reads source x=3. |
| `channels` | R/G/B 3×5; odd/even asymmetric kernels and independent anchors match scalar convolution exactly | Replace one kernel binding. G-only reads input0 and input2; input1/input3 have empty dirty influence on G. |
| `gaussian` | Default kernel 5×5; image 3×5×3; extracted R → `field.convolve` equals image R bit for bit | Change radius/sigma, or feed `kernel` into another graph branch. Kernel-only has zero image source reads. |

Every successful invocation ends with `multi-output oracle=passed`. Radius 0,
0.25, 1, 1.25, 2, 64 produces side lengths 1, 3, 3, 5, 5, 129 respectively.
Sigma zero yields a center impulse of the same shape. Integer-neighbor floats,
NaN/infinity/range rejection, independent cache/dirty behavior, cancellation and
joint protocol failures have additional deterministic integration tests.

To request one output, keep only that named entry in `WorkflowDocument::outputs`
(as shown for Y, G and kernel), or use a frozen plan with
`ExecutionContext::execute_fragments` and a selected `DemandQuery`. A semantic
node can retain multiple output declarations while only needed physical result
steps execute. Port names and shapes are resolved at compilation; runtime-length
port lists are not supported. RequestRecord outputs use their singleton protocol.

Exact operator parameter and Region contracts are in
[Multi-Output-Operations](../../docs/kernel-architecture/Multi-Output-Operations.md).
