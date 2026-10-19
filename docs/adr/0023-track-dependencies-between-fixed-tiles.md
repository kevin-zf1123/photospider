# ADR 0023: Track Dependencies Between Fixed Tiles

- Status: Accepted
- Reader mirror: [Chinese](zh/0023-track-dependencies-between-fixed-tiles.zh.md)
- Current boundary: package 0.36.0 implements per-member tile grids, member-local interval Needs, and Run-shared stages as declared in §3. A producer's semantic descriptor is its resolved static Result schema; Results carry no facets or attached resource identities.
- Related: [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md), [ADR 0017](0017-cpu-regional-execution-and-storage.md), [ADR 0018](0018-local-result-caches.md), [ADR 0021](0021-independent-node-results.md), [ADR 0022](0022-freeze-operation-c-abi-v3.md)

## 1. Core summary (TL;DR)

Photospider computes and tracks work in fixed tiles of individual Tensor members instead of recording a sample-level dependency relation with support roles and guarantee levels. One plan-wide tile extent cuts every member of every Result into its own grid; each requested member tile is computed by one task, and the coordinator records which producer member tiles each computation requested. Dirty propagation, completed-result reuse and binding replacement operate on these tile records, which removes the sample relation, its roles and guarantee levels, and the exact-set algebra that supported them.

## 2. Mental model and intuition

A tile of one member is the smallest unit that is published, cached and reported dirty. The plan fixes one square tile extent; every member is cut with that extent along its own tiled axes, so each member has its own tile count and index space. Each computation records the producer member tiles it requested through Needs; the records form a tile-level graph beside the node graph.

The example below is an STMap whose `uv` input declares a spatial layout with the same height and width as the output.

```text
caller requests tile (1,1) of member 0 of "warped"
        |
  warped[0]: tile (1,1) -----------------------+
        |  poll 1: Need uv[0] tile (1,1)       |
        v                                      |
  uv[0]:  tile (1,1) computed                  |  record for warped[0] (1,1):
        |  poll 2: Need source[0] tiles        |    uv[0]     -> {(1,1)}
        v  (computed from the uv values)       |    source[0] -> {(0,2), (1,3), (3,0)}
  source[0]: tiles (0,2) (1,3) (3,0) computed -+
        |
        v
  warped[0] (1,1) computes all of its pixels; payload, coverage and record commit together
```

The same records answer three questions without reading pixels:

- **Dirty:** an edit to source tile (1,3) marks every consumer tile whose record names (1,3), then continues through those consumers' dependents.
- **Reuse:** a completed tile is reusable while its physical identity and the content of every workflow-input tile reached through its record are unchanged.
- **Binding replacement:** the coordinator compares the new input content tile by tile; changed input tiles seed the dirty propagation.

## 3. Formal contracts and interfaces

`PlanInput` is the `std::variant<PlanStepInput, PlanWorkflowInput>` from `photospider/compiler/compiler.hpp`. The declarations below are excerpts of `photospider/data/tiles.hpp`, `photospider/execution/tile_dependencies.hpp` and `photospider/compiler/compiler.hpp`; constructors and member functions are omitted.

```cpp
namespace ps {

struct PlanningOptions final {
  // ...
  std::map<std::string, TileRequest> output_requests;
  std::uint64_t tile_extent = 128;  // positive power of two, including 1
};

using TileIndex = std::uint64_t;
struct TileInterval final { TileIndex begin = 0, end = 0; };  // [begin,end)
class TileSet;  // normalized, Root-charged interval list

class TileGrid final {  // one per member
 public:
  static Result<TileGrid> from_tensor(const ResultTensorSpec& tensor,
                                      std::uint64_t tile_extent = 128);
  std::uint64_t tile_extent() const noexcept;
  std::uint64_t batch_count() const noexcept;
  std::uint64_t tile_rows() const noexcept;
  std::uint64_t tile_columns() const noexcept;
  TileIndex tile_count() const noexcept;
  Result<Region> region(TileIndex tile) const;
};

struct TileRequest final {  // member slot -> tiles
  std::map<std::uint32_t, TileSet> members;
};

struct TileNeed final {  // one input request from a start or poll callback
  std::uint32_t input = 0, tensor_slot = 0;
  bool descriptor_only = false;
  TileSet tiles;
};

struct TileUnit final {
  std::size_t step = 0;
  std::uint32_t output = 0, tensor_slot = 0;
  TileIndex tile = 0;
};

struct TileDependency final {
  PlanInput producer;
  std::uint32_t producer_output = 0;  // unused for a workflow input
  std::uint32_t tensor_slot = 0;
  TileSet tiles;  // empty: semantic descriptor only
};

struct TileRecord final {
  TileUnit unit;
  ResourceVector<TileDependency> inputs;
  ResourceVector<std::shared_ptr<const TileEvidence>> shared;
};

}  // namespace ps
```

### 3.1 Member tile grids

Every member of every Result has a grid. `TileGrid::from_tensor()` derives it from the member's resolved `ResultTensorSpec` and the plan's `tile_extent`:

- **Spatial member** (`ResultTensorLayout::spatial == true`): tiles partition the declared `height_axis` and `width_axis`. Every explicit batch coordinate has its own tiles, and every other cell axis, including the channel axis, is covered completely by each tile. Axis order is taken from the layout; `{N,L,H,W,C}` and `{C,H,W}` are both valid examples.
- **Nonspatial member:** the divisible axes are the non-atomic cell axes. Tiles partition the last two such axes; earlier non-atomic cell axes and all explicit batch axes act as batch coordinates. A member with one divisible cell axis is tiled along that axis only. With no divisible cell axis, each batch coordinate forms one tile. A scalar or atomic tuple without batch axes forms one tile. Atomic trailing axes are covered completely by each tile.

For a positive tile extent `T` and a tiled extent `E`, the tile count along that axis is:

$$
C = \left\lceil \frac{E}{T} \right\rceil.
$$

```cpp
// Preconditions: tile_extent > 0. Avoids evaluating extent + tile_extent - 1.
const std::uint64_t count = extent / tile_extent + (extent % tile_extent != 0);
```

The flat tile index is `(batch_index * tile_rows + tile_row) * tile_columns + tile_column`. Edge tiles are clipped to the member bounds. Grid creation rejects a tile count that does not fit `TileIndex`, without requiring the member's full sample count to fit `uint64_t`.

A producer is either a plan step output or a workflow input. A workflow input is an input declared by the top-level `WorkflowDocument` and bound by the host for each Run; its grids are derived at compile time from its declared Result schema and do not depend on how the bound Result is stored. A plan step output's grids are derived from its resolved output schema. The units of every producer are its member tiles; physical storage partitions are not dependency keys.

Only the top-level document declares workflow inputs. Authoring helpers, and any composite operation added later, expand into ordinary nodes before analysis; the inputs of an expanded fragment become edges to the producers that feed it and follow those producers' rules.

### 3.2 Requests, computation and inputs

Callers and operations address work by member slot and tile intervals. `PlanningOptions::output_requests` maps a named output to a `TileRequest`; a missing name selects every member through compact intervals, and an empty request stays empty. Unknown names, member indices and out-of-grid intervals fail before publication. One task computes each requested member tile completely. The grid of every member is available to callers from the plan and to callbacks through their query.

Inputs reach a computation only through Needs. The coordinator does not prefetch inputs, and the static active-input projection produced by preparation is unchanged: an input outside the projection is never requested or executed. A task can issue Needs in several polls and choose later Needs from values it has already read, as STMap and the LUT bake gate do.

A `TileNeed` names an input port and one of two request forms:

- **tiles:** `descriptor_only == false`, a member `tensor_slot`, and a set of intervals in that member's grid. An empty set requests nothing: the coordinator creates no dependency, no descriptor grant and no producer work.
- **descriptor only:** `descriptor_only == true`, `tensor_slot == 0` and an empty `tiles`. It grants the semantic descriptor without payload.

The operation's query carries every member grid of every input, and the operation itself converts the sample coordinates it needs into tile intervals; SDK helpers provide that conversion. The coordinator validates the complete Need envelope before it starts any producer work. For each nonempty tile Need it:

1. rejects an interval outside the producer member's grid;
2. schedules the requested producer member tiles that are not yet published, without occupying a worker while it waits;
3. grants read access to the requested tiles, closed over `atomic_trailing_axes`, together with the producer's descriptor facts;
4. records the request in the `TileDependency` for that producer member.

The record of a task is the union of its Needs within that task and binding generation; intervals merge only within one producer member. Every input read passes through a Need, so the record is complete by construction; it may name tiles whose samples the computation did not use.

An algorithm that needs a complete input requests every tile of every member it reads. An algorithm that computes all outputs in one pass, or that must produce an externally visible effect once, declares a Run-shared stage: a static continuation that the coordinator instantiates at most once per Run, node and stage. Consumers request the stage's committed member tiles through `ResultSharedNeed`, and each stage commit carries frozen `TileEvidence` that consumers inherit. Inherited evidence never grants read access to the stage's original inputs.

### 3.3 Validation scope

The host checks schema and port constraints over every tile it grants and every tile it publishes. An operation applies its own numeric constraints over the complete output tile it computes. A non-finite value is an error only where the schema or the operation contract requires finite values. A validator that requires complete coverage sees it only when the requested and scope-closed coverage is complete; static publication scopes may group members for visibility, but they do not widen any input Need.

### 3.4 Publication and storage

A tile becomes visible only when its payload, its coverage and its dependency record commit together. A validation, budget or cancellation failure installs no partial tile and no record. A published tile is immutable. Static publication scopes can delay visibility of a completed tile until a `CompleteBundle` is complete or until earlier tiles of a `StablePrefix` member are visible; completing a tile alone does not establish complete coverage of its member. A tile may be published as a view that retains a larger backing owner; Root accounting follows the backing owners that are actually retained, so evicting one tile does not necessarily release a tile-sized allocation. Run failure, cancellation and the lifetime of already published data follow ADR 0017.

### 3.5 Semantic descriptors and production progress

A producer's semantic descriptor is its resolved static Result schema: the schema id and version, the 1..16 member element types, shapes, spatial layout topology, atomic axes, domain and publication scopes. Every member shape is fixed before execution, so no descriptor fact depends on produced data. The descriptor describes structure; Results carry no attached domain label. Resources that operation preparation selects by explicit identity enter the consuming step's physical identity (§3.8), and configuration stays with the step that uses it rather than traveling with a numeric Result. Object identity, publication revision, current coverage and the sealed flag record production progress. Publishing another tile changes progress only and dirties nothing. A change to the semantic descriptor dirties every record that names the producer. A shape or layout change that no longer fits the plan requires preparation and planning again; tile indices from the old plan are not reused.

### 3.6 Record lifecycle

- A successful recomputation replaces the unit's previous record and updates the reverse index, so a producer tile that is no longer read stops propagating dirty to it.
- A failed recomputation leaves the previous generation's tile and record unchanged.
- A task from an older binding generation cannot install its record into a newer generation.
- A completed-tile hit or checkpoint hit restores the dependencies of the computation it skips, including inherited shared-stage evidence. A block hit restores only block state; the current actor's Needs remain in effect. Block keys keep their existing constraints on input state and content.

### 3.7 Dirty propagation

The coordinator keeps a reverse index from each producer member tile, and from each shared evidence node, to the records that name it. A tile dependency matches a change to any of its tiles and a change to the producer's semantic descriptor; a descriptor-only dependency matches a semantic descriptor change. A dirty query starts from the changed workflow-input tiles, visits each reachable record once, and returns the dirty units per output as member slot and tile index.

Replacing the binding of a workflow input compares the new content tile by tile and seeds only the changed tiles. A host that implements painting binds the painted layer as a workflow input, so a stroke dirties only the tiles it touches and their dependents. During dirty propagation, the work is bounded by the records and dependencies reachable from the changed inputs. Binding content comparison and content identity computation are charged separately by the tiles and logical data they actually inspect; both are charged to the Root budgets.

### 3.8 Completed-tile reuse

A computed tile's identity combines the existing physical identity of its step, which includes the tile extent, the resolved output schema, the implementation, the backend profile and the resource identities that affect the result, with the member slot, the tile index and the content of every workflow-input tile reached through its record. Changing the tile extent therefore never reuses an old tile, including tiles of operations without inputs. Input content identity is computed over logical elements and the semantic description; padding, stride, plane order and storage addresses do not participate.

## 4. Non-goals and explicit boundaries

- Sample-level dependency evidence, support roles (Data, Control, Validation, Descriptor) and guarantee levels (Exact, Conservative, Unknown) are not part of the model.
- The smallest unit that is published, cached or reported dirty is one member tile. Requests for part of a tile compute the whole tile.
- Kernel execution has no whole-result mode and no producer unit that stands for a complete Result. Whole-output algorithms run inside Run-shared stages or SDK adapters, and their consumers still request member tiles.
- One square tile extent applies to the whole plan; operations cannot declare their own tile size or tile shape.
- Results have no fields, runtime member counts or dynamic tile End; every member's grid is fixed at compile time.
- Composite or nested workflows are not kernel nodes; they expand into ordinary nodes before analysis.
- The kernel provides no static region rules such as Elementwise, Halo or Shrink. Operations compute their own Needs; SDK helpers may provide those formulas.
- Records describe one plan and one binding generation; they are not a persistent format.

## 5. Consequences

**Extra computation.** A caller that needs a small region requests every tile it touches and receives complete tiles. A consumer tile that reads a few samples from many producer tiles requests each of those producer tiles, and each is computed completely. An algorithm that needs a complete input requests every input tile before it can compute, and a Run-shared stage holds its complete result for the remainder of the Run.

**Wider error domain.** Because a tile is computed completely, an invalid value anywhere in the tile's computation fails the tile and the request that needs it, where the schema or operation requires valid values. For STMap, a non-finite UV value at an unrequested pixel inside a requested tile fails the request. Host schema checks cover every granted producer tile, including samples no tap uses.

**Coarser invalidation.** An edit dirties every consumer tile that requested any part of an edited producer tile, and every tile downstream of those. A consumer of a Run-shared stage that read its complete input is dirtied by any change to that input. After edits the Run recomputes more than an exact sample relation would require, and completed-tile reuse misses correspondingly more often.

**Record memory.** The cost of a record grows with the number of interval boundaries it names, not with the number of tiles. A dense bitset over the producer's whole index space would cost $\lceil P/64 \rceil$ words regardless of how many tiles are set: for 100 frames of 7680-by-4320 with 128-pixel tiles, `P = 204000` and each such bitset takes 25,504 bytes, which would reach about 4.85 GiB for one dependency per consumer tile. `TileSet` therefore stores normalized half-open intervals and charges their allocation, copying and scanning to the Root work and metadata budgets; exceeding them returns `ResourceExhausted`. Shared-stage evidence is stored once per commit and referenced, so P stage tiles consumed by Q tiles do not create P by Q copies.

**Tile size trade-off.** Smaller tiles increase the number of tasks, records and scheduling operations; larger tiles increase wasted computation per request and dirty-set size per edit. The default extent of 128 remains until measurements justify a change.

**Removed mechanisms.** The sample relation and its constructors, packed Gather evidence, per-sample dependency certificates, support roles and guarantee levels, exact-set normalization used as dependency evidence, tuple closure as dependency evidence, query-scoped dependency bundles and their rebinding, request-keyed actors, whole-result execution, whole Needs, and Result fields are removed. Structural and channel consistency checks that operations still need remain as schema validation. Multi-stage Needs, the static input projection, Root resource admission, cancellation and callback retirement remain.

**Binary interface.** Needs carry a member slot with half-open tile intervals, or a descriptor-only request; publication carries no relation. The Result operation C ABI 6 carries these records. The frozen prefix-negotiated interface remains the separate target of ADR 0022.
