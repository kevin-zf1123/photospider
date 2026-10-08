# Paged labels → area index → filter

This public-API example registers `components4.labels`, `components4.area`, and `components4.filter`. It binds an immutable HW mask Result directly. The compiled DAG publishes Complete Components, area-index, and filtered Results. A sink checks the filtered pixels and publishes a one-tensor Float64 Result. An independent breadth-first traversal checks every label, component `[id,area,min]` row, and filtered pixel. The oracle uses ordinary caller-owned vectors, outside the product's managed-capacity accounting. C++ fixture variants select the bound mask contents and backing layout; they are not graph inputs or source operations.

```sh
cmake --build build/kernel-dev --target photospider_components_workflow test_component_contract -j 8
ctest --test-dir build/kernel-dev -R '^(test_component_contract|example_components_workflow)$' --output-on-failure
```

An isolated consumer of an installed package:

```sh
cmake --install build/kernel-dev --prefix "$PWD/out/components-install"
cmake -S examples/components_workflow -B out/components-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/components-install"
cmake --build out/components-consumer -j 8
out/components-consumer/photospider_components_workflow
out/components-consumer/photospider_components_workflow --large
```

The standalone CMake project requires a compatible Photospider 0.32 package.

## Result inputs and ownership

Labels accepts one unbatched, facet-free UInt8 HW tensor Result with no fields. It validates the tensor type and shape without fixing the numeric input Result schema ID. Area consumes a complete Components Result; Filter consumes matching Components and area-index Results. Fixture setup creates caller-owned mask backing with `BufferAllocator`, imports it through `root.reference()`, and binds the immutable Result. Ordinary, negative-stride, and constant zero-stride layouts use the same tensor schema. The foreign-index case binds a second, distinct `other_mask` Result to check association identity.

Area copies its `(id,area)` rows into its own Result fields. Result associations store source ObjectIds and do not retain source payload. A loaded field `CpuStorage` retains its read plan and Result implementation; the bytes remain readable after the Result wrapper and execution context are gone, and their backing is released with the final loaded window.

Fixture setup creates the full caller-owned mask backing with `BufferAllocator`; `root.reference()` charges those bytes to Referenced. Host and Payload peaks exclude these input bytes. The Root Referenced cap is `2*H*W` bytes for the bound mask Results. Host and Metadata capacities are 1 MiB when `H*W<=65,536` and 4 MiB otherwise, and Payload is 32 KiB. The executable prints `referenced_peak`, checks Host and Payload peaks against their configured limits, and confirms live Referenced and Disk usage return to zero after the last field window is released. These are fixture limits, not constraints imposed by the operation factory.

## Independent expected results

Nonzero UInt8 values are foreground. Connectivity uses four neighbours in the
original HW image, regardless of source/output page boundaries. Background ID
is zero; each component ID is one plus its minimum row-major pixel position.

| Mask | Expected component rows `[id,area,min]` |
| --- | --- |
| `000 / 000` | `[]` |
| `001 / 100` | `[3,1,2], [4,1,3]` |
| `11 / 11` | `[1,4,0]` |
| `101 / 111` | `[1,5,0]` |
| UInt8 `[0,2,255,0,7]` | `[2,2,1], [5,1,4]` |
| `1011011 / 1011111 / 1110000` | `[1,14,0]`, even when the union root address is 3 |
| 2×127, even columns in first row and all of second row | `[1,191,0]` |

For every pixel, the independent expected filter is
`label!=0 && area[label]>=minimum_area`. A positive Int64 threshold is required,
including INT64_MAX; an empty component set yields a complete all-zero mask.
`maximum_count` limits final K, so the empty case also passes with maximum_count
zero, and the comb passes with maximum_count one.

The BFS code is in `main.cpp`; it uses a queue and visits all four neighbours.
It does not reuse the product's rank-union, root-minimum or binary-search code.
All comparisons are exact integer/byte comparisons. A standalone check of the
cross-page comb can be run with Python's standard library:

```sh
python3 - <<'PY'
from collections import deque
h,w=2,127
mask=[int(y==1 or x%2==0) for y in range(h) for x in range(w)]
labels=[0]*(h*w); rows=[]
for seed in range(h*w):
    if not mask[seed] or labels[seed]: continue
    q=deque([seed]); labels[seed]=seed+1; area=0
    while q:
        p=q.popleft(); area+=1; y,x=divmod(p,w)
        for yy,xx in ((y-1,x),(y+1,x),(y,x-1),(y,x+1)):
            if 0<=yy<h and 0<=xx<w:
                j=yy*w+xx
                if mask[j] and not labels[j]: labels[j]=seed+1; q.append(j)
    rows.append((seed+1,area,seed))
assert rows==[(1,191,0)]
print(rows)
PY
```

## Resource and ownership checks

The product initializes one mandatory 32-byte union record per logical pixel,
then processes left/top edges. The comb therefore starts with **191 foreground
self-roots** in 8128 logical UF bytes, rounded for disk admission. The design's
65 temporary run components are a different illustrative counting schedule.
Provisional storage is not inferred from the final one-row component table.

The executable covers 32/64/256-byte windows, 528 isolated components with a paged binary-search area index, and a 100×100 connected input. The connected fixture contains 80,000 bytes of labels and 320,000 bytes of private union-find records. The default cases use 1 MiB each for Root Host and Metadata. The default run also uses a 20,000-stage per-continuation limit for a 100×100 connected fixture; `--stage-regression` stops after this case.

`--large` configures a 1000x1000 all-background image (`K=0`, `maximum_count=0`) and an all-foreground image (`K=1`, `maximum_count=1`). The latter has the sole component row `[1,1000000,0]`; threshold 1000000 preserves every pixel. Both use 1024-byte windows, 4 MiB Host/Metadata limits, a 32 KiB Payload cap, 200 million Run work units, and a one-million-stage per-continuation limit. The independent BFS reference checks all labels, area rows, and filtered bytes. Printed `issued_stages` counts coordinator actions across the DAG, not any individual producer's polls. Actual page-padding writes and the complete association validator consume additional I/O and work beyond logical field sizes.

The run reports Root Host, Payload, Referenced, and aggregate `issued_stages` counters. The stage count is coordinator activity across the workflow, not an individual continuation's poll count. Root counters are not process RSS. Components operations use CPU stages; this example makes no GPU claim.

Duplicate Labels nodes start once with the optional dependency cache disabled. Result associations store source ObjectIds but do not retain source payload. A loaded filter field window retains its own read plan, Result implementation, and backing after the Result wrapper and context retire; releasing the final window reclaims that field storage. Count/page/work/Disk failures, cancellation at Labels publication after Disk backing exists, and stale plans cover cleanup; a stale plan is rejected before any operation starts. A foreign index from a same-shaped labelset is rejected.

`test_component_contract` additionally injects short, reordered and corrupted
indices through public callbacks. It checks complete index validation,
Association scope and the offending index ObjectId, and rejects a foreign index
even when its numeric contents happen to match. Count/basis validation alone
does not prove connectivity of an arbitrary imported labelset.

These are managed-capacity and exact fixture checks, not process RSS bounds.
See [the runtime contract](../../docs/kernel-architecture/Paged-Components.md).

Their Root Host peaks were 244,437 and 236,485 bytes, Payload peaks were 9,600 and 9,624 bytes, Referenced peak was 1,000,000 bytes for each case, and aggregate `issued_stages` were 150,929 and 213,400. These counters are managed Root usage, not process RSS.
