# Paged labels → area index → filter

This installed-public-API example registers `components4.labels`,
`components4.area` and `components4.filter`, then executes UInt8 mask source →
compiled DAG → a binary pixel sink. An independent breadth-first traversal
checks every label, component `[id,area,min]` row and filtered pixel. The oracle
uses ordinary caller-owned vectors; those reference buffers are not part of the
product's managed-capacity measurements.

```sh
cmake --build build/issue257-shared --target photospider_components_workflow test_component_contract -j 8
ctest --test-dir build/issue257-shared -R '^(test_component_contract|example_components_workflow)$' --output-on-failure
```

An isolated consumer of an installed package:

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-delivery/install"
cmake -S examples/components_workflow -B out/phase-a-delivery/components-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/components-consumer -j 8
out/phase-a-delivery/components-consumer/photospider_components_workflow
```

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

The executable covers 32/64/256-byte windows, 528 isolated components with a
paged binary-search area index, and a 100×100 connected input. Its 80,000-byte
labels and 320,000-byte private UF exceed the **65,536-byte managed Host limit**.
The last case measured a 38,273-byte Host peak in local validation. Actual page
padding writes and the complete association validator consume additional I/O
and work, beyond logical field sizes.

Duplicate labels nodes must start once with the optional cache disabled. The
final filter read window retains labels and area after the context and output
ResultRefs are destroyed, and final release reclaims backing. Count/page/work/
disk failures, cancellation after temporary writes and stale plans check
Disk/Payload cleanup. A foreign index from a same-shaped labelset is rejected.

`test_component_contract` additionally injects short, reordered and corrupted
indices through public callbacks. It checks complete index validation,
Association scope and the offending index ObjectId, and rejects a foreign index
even when its numeric contents happen to match. Count/basis validation alone
does not prove connectivity of an arbitrary imported labelset.

These are managed-capacity and exact fixture checks, not process RSS bounds.
See [the runtime contract](../../docs/kernel-architecture/Paged-Components.md).
