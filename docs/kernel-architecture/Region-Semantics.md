# Region semantics

## Scope & Ownership

A `Region` describes logical sample coverage in a complete tensor descriptor. Storage origin, byte offset, strides, padding, and physical planar pages describe how those samples are stored; they do not change the Region. The planner, executor, and storage owner each retain their own part of this contract: planning authorizes dependencies, execution passes exact demands, and storage bounds readable or writable samples.

## Data Layout & Memory

```cpp
#include <cstdint>
#include <vector>

#include "photospider/data/region.hpp"

ps::Region whole_region(const std::vector<std::uint64_t>& shape) {
  return ps::Region::whole(shape);
}
```

Each dimension is a half-open interval `[offset, offset + extent)` within its descriptor axis. The constructor validates dimensions and checked endpoints. A tensor of shape `(height, width, channel)` can request a pixel rectangle while retaining all four channel samples for each RGBA pixel. Planar storage can place channels in separate planes; complete logical channel coverage does not imply interleaved physical bytes.

An input view separately exposes storage origin, offset, signed strides, valid coverage, and callback demand. A callback may address only samples inside the authorized demand and valid coverage, using the supplied storage mapping. Output publication records its descriptor and exact produced Region. Empty coverage has no sample reads.

## Execution & State

The planner derives dependencies from each operation's declared rule. Whole operations demand complete inputs. Elementwise operations map matching coordinates. Halo operations expand demand and clip it to the complete image boundary with checked arithmetic. Shrink maps ceil-divided output coordinates to clipped input boxes. Shape and dependency propagation reject unknown axes or ports, out-of-domain rectangles, and partial channel coverage where an image port requires complete channels.

Planning options retain each output name's exact requested Region and positive tile dimensions. Changing either replans optimized IR. Distinct names remain part of plan identity even when they alias one node; runtime payload bytes do not enter plan identity. A tile plan derives a dependency-pruned subplan for one region without allocating the complete tile grid. Neighboring tiles may recompute overlapping halo. Whole, nondeterministic, and side-effecting boundaries materialize complete results once in topological order.

The regional Value executor returns requested coverage. With no requested region, it requests complete output. Ordinary dense Value bindings provide complete snapshots; `RegionalSource` copies its metadata and callable for the Run and fills host-provided packed region storage. It must support concurrent immutable reads, observe cooperative cancellation, and report exactly the requested Region. Source and operation callbacks must not synchronously reenter execution on workers owned by the same context. This path handles Value dependencies; structural planar operations use the distinct planar executor described in [Data Model](Data-Model.md) and [Compiler and Execution](Compiler-and-Execution.md).

Value collection packs requested coverage while preserving the complete logical descriptor. Structural planar collection instead requires `PlanarImage` bindings whose descriptors, facets, layout, and tile geometry match the compiled declaration. Planar image execution rejects Value, `RegionalSource`, and snapshot bindings for planar inputs. `execute_stream` accepts the Value sink path and returns `TypeMismatch` for plans that require structural planar execution.

`execute_stream` invokes its required sink synchronously on the execute caller thread. It passes borrowed `ValueView` objects that expire when the sink returns. Eligible deterministic, side-effect-free CPU dependency streams can prepare a bounded window of later tiles while the caller delivers the front tile; the window is limited by execution parallelism. Sink calls remain ordered, and a blocked sink stops further delivery while already admitted window work may finish. A sink failure stops later delivery and drains admitted work; already delivered tiles cannot be revoked. Collected failure returns no partial result.

Cancellation and currentness checks guard admission, callback entry and completion, sink calls, and final assembly. At entry, Stale takes precedence over binding validation. After entry, Cancelled takes precedence over Stale and ordinary errors. All admitted callbacks retire before borrowed source/output storage is reused.

## Algorithms & Math

For a positive tile size `T` and extent `E`, the planner computes the tile count with checked ceil division:

$$
C = \left\lceil \frac{E}{T} \right\rceil,
\qquad
C = E / T + (E \bmod T \ne 0).
$$

The integer form avoids `E + T - 1` overflow. Bounds and shape products are checked before allocation. Empty extents produce no sample work. Numeric parameter schemas can declare finite inclusive bounds; bounded Int64 values remain within exact binary64 integer range, and bounded Float64 endpoints must be finite. A Halo operation may resolve its radius from a required bounded Int64 parameter.

## Limitations & Non-Goals

- Regional execution does not make an operation region-capable when its declared rule requires whole inputs.
- Collected outer tile traversal is sequential. Eligible deterministic CPU dependency streaming uses a bounded concurrent tile window; other stream and regional paths follow their own execution rules.
- Caller-owned input payload is outside the computation `maximum_live_bytes` payload sublimit. When a managed resource root is enabled, the kernel admits a reference lease for external Value storage under the separate Referenced capacity dimension. Source-private external state, thread stacks, and process RSS remain outside the controlled allocation bound.
- Streaming sinks receive irrevocable tiles before final stream success. Callers that need rollback must stage their own output.
- `RegionalSource` is a C++ execution binding; it does not add a source codec or provider-ABI extension.
