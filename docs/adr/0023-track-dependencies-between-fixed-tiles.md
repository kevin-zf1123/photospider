# ADR 0023: Track Dependencies Between Fixed Tiles

- Status: Accepted
- Reader mirror: [Chinese](zh/0023-track-dependencies-between-fixed-tiles.zh.md)
- Current boundary: in 0.34.0 the facets and resource identities that the original §3.5 placed in a producer's semantic descriptor are superseded; the descriptor is the resolved Result schema plus fixed global facts.
- Related: [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md), [ADR 0017](0017-cpu-regional-execution-and-storage.md), [ADR 0018](0018-local-result-caches-and-frozen-execution.md), [ADR 0021](0021-independent-node-results.md), [ADR 0022](0022-freeze-operation-c-abi-v3.md)

## 1. Core summary (TL;DR)

The executor currently records, for every output sample, which input samples support it, with four support roles and three guarantee levels, and answers dirty, cache and binding-replacement queries by projecting that sample relation. Photospider will compute and track work in fixed square tiles instead: one plan-wide tile extent partitions the spatial axes of every spatial tensor, a Tiled output is computed one complete tile at a time, a Whole output is computed by one task that publishes all of its tiles, and the coordinator records which producer tiles, or which complete producers, each computation requested. Dirty propagation, completed-result reuse and binding replacement then operate on these tile records, which removes the sample relation, its roles and guarantee levels, and the exact-set algebra that supports them.

## 2. Mental model and intuition

A tile is the smallest unit that is published, cached and reported dirty. The plan fixes one square tile extent; every spatial tensor is cut along its own height and width axes with that extent, so each tensor has its own tile count and index space. Each computation records the producer tiles it requested through Needs; the records form a tile-level graph beside the node graph.

The example below is an STMap whose `uv` input declares a spatial layout with the same height and width as the output.

```text
caller requests tile (1,1) of "warped"
        |
  warped: tile (1,1) --------------------------+
        |  poll 1: Need uv tile (1,1)          |
        v                                      |
  uv:     tile (1,1) computed                  |  record for warped (1,1):
        |  poll 2: Need source tiles           |    uv      -> {(1,1)}
        v  (computed from the uv values)       |    source  -> {(0,2), (1,3), (3,0)}
  source: tiles (0,2) (1,3) (3,0) computed ----+
        |
        v
  warped (1,1) computes all of its pixels; payload, coverage and record commit together
```

The same records answer three questions without reading pixels:

- **Dirty:** an edit to source tile (1,3) marks every consumer tile whose record names (1,3), then continues through those consumers' dependents.
- **Reuse:** a completed tile is reusable while its physical identity and the identities of every producer it recorded are unchanged.
- **Binding replacement:** the coordinator compares the new input content tile by tile; changed input tiles seed the dirty propagation.

## 3. Formal contracts and interfaces

The following declarations are the proposed target, not current code. `PlanInput` is the existing `std::variant<PlanStepInput, PlanWorkflowInput>` from `photospider/compiler/compiler.hpp`.

```cpp
namespace ps {

// Plan-wide square tile extent: a positive power of two, including 1.
struct TileGrid final {
  std::uint64_t tile_extent = 128;
};

enum class OperationRegionRule : std::uint32_t {
  Whole = 1U,  // one task computes the complete output
  Tiled = 2U,  // one task per output tile
};

// Flat tile index inside one tensor's own grid:
// (batch_index * tile_rows + tile_row) * tile_columns + tile_column
using TileIndex = std::uint64_t;

// A set of tile indices of one producer tensor. The representation is an
// implementation choice; its storage and work are charged to the Root.
class TileSet;

struct TileDependency final {
  PlanInput producer;                 // plan step output or workflow input
  std::uint32_t producer_output = 0;  // unused for a workflow input
  bool whole = false;  // complete producer: every unit and its semantic descriptor
  TileSet tiles;       // requested tiles of a Tiled producer;
                       // empty and !whole: semantic descriptor only
};

// One input request issued from a start or poll callback.
struct TileNeed final {
  std::uint32_t input = 0;       // input port
  bool whole = false;            // the complete producer
  bool descriptor_only = false;  // the semantic descriptor without payload
  TileSet tiles;                 // tile indices in the producer's own grid;
                                 // empty when either flag is set
};

struct TileRecord final {
  std::uint32_t step = 0;
  std::uint32_t output = 0;
  TileIndex tile = 0;  // unit index: a tile of a Tiled output, 0 for Whole
  std::vector<TileDependency> inputs;
};

}  // namespace ps
```

### 3.1 Spatial tensors and the tile grid

A tensor is spatial when its resolved `ResultTensorLayout` has `spatial == true`. Its tiles partition the declared `height_axis` and `width_axis` with the plan's `tile_extent`. Every batch coordinate has its own tiles, and every other cell axis, including the channel axis, is covered completely by each tile. Axis order is taken from the layout; `{N,L,H,W,C}` and `{C,H,W}` are both valid examples.

For a positive tile extent `T` and a spatial extent `E`, the tile count along that axis is:

$$
C = \left\lceil \frac{E}{T} \right\rceil.
$$

```cpp
// Preconditions: tile_extent > 0. Avoids evaluating extent + tile_extent - 1.
const std::uint64_t count = extent / tile_extent + (extent % tile_extent != 0);
```

Edge tiles are clipped to the spatial bounds. The planner rejects a grid whose tile count for any tensor does not fit `TileIndex`.

A producer is either a plan step output or a workflow input. A Result has a tile grid when it contains exactly one tensor, that tensor is spatial, and the Result has no fields.

- A plan step output is Tiled when the operation declares `Tiled` and the resolved output Result has a tile grid. The rule is decided from the resolved output schema, so one operation can produce a Tiled output for an image and a Whole output for a non-spatial tensor.
- A workflow input is an input declared by the top-level `WorkflowDocument` and bound by the host for each Run. It has no operation; it is Tiled when its declared Result schema has a tile grid. The decision is made at compile time from the declaration and does not depend on how the bound Result is stored.
- Every other producer is Whole.

The units of a Tiled producer are its tiles. A Whole producer has exactly one logical unit, index 0, which is its only key for computation, dependency records, caching and dirty propagation. The storage tiles inside a Whole output are physical layout and are not dependency keys.

Only the top-level document declares workflow inputs. Authoring helpers, and any composite operation added later, expand into ordinary nodes before analysis; the inputs of an expanded fragment become edges to the producers that feed it and follow those producers' rules.

### 3.2 Requests, computation and inputs

Callers and operations address work by tile index; the kernel does not map rectangles to tiles. A caller requests a `TileSet` of a named Tiled output, or the complete output of any named output. For a Tiled output one task computes each requested tile completely. For a Whole output a nonempty request runs one task that computes the complete output and publishes all of its tiles at once. An empty request keeps the existing short-circuit rules. The grid of every output is available to callers from the plan: the tile extent, the batch count and the tile rows and columns of each spatial tensor.

Inputs reach a computation only through Needs, for Whole and Tiled tasks alike. The coordinator does not prefetch inputs, and the static active-input projection produced by preparation is unchanged: an input outside the projection is never requested or executed. A task can issue Needs in several polls and choose later Needs from values it has already read, as STMap and the LUT bake gate do.

A `TileNeed` names an input port and exactly one of three requests:

- **tiles:** a nonempty set of tile indices in the producer's own grid, with both flags false;
- **whole:** the complete producer, with `whole == true`, `descriptor_only == false` and an empty `tiles`;
- **descriptor only:** the semantic descriptor without payload, with `descriptor_only == true`, `whole == false` and an empty `tiles`.

Any other combination, including a Need with both flags false and no tiles, is rejected with `InvalidArgument`. An empty tile set never stands for a whole or descriptor-only request. The operation's query carries the grid of each input that has one, and the operation itself converts the sample coordinates it needs into tile indices; SDK helpers provide that conversion. For each Need the coordinator:

1. rejects a tile index outside the producer's grid, and a tile request to a producer without a tile grid, which accepts only whole or descriptor-only requests;
2. normalizes a tile request to a Whole producer that has a tile grid into a whole request;
3. schedules the requested producer units that are not yet published, without occupying a worker while it waits;
4. grants read access to the requested units; a whole request grants the complete producer;
5. records the request in the `TileDependency` for that port: the requested tiles of a Tiled producer, `whole == true` for a whole request, or an empty set with `whole == false` for a descriptor-only request.

The record of a task is the union of its Needs within that task and binding generation. Every input read passes through a Need, so the record is complete by construction; it may name tiles whose samples the computation did not use. The storage tiles of a Whole output share the single record of its unit 0.

### 3.3 Validation scope

The host checks schema and port constraints over every unit it grants, that is, complete tiles or complete outputs. An operation applies its own numeric constraints over the complete output tile or Whole output it computes. A non-finite value is an error only where the schema or the operation contract requires finite values.

### 3.4 Publication and storage

A tile becomes visible only when its payload, its coverage and its dependency record commit together. A validation, budget or cancellation failure installs no partial tile and no record. A published tile is immutable. A tile may be published as a view that retains a larger backing owner; Root accounting follows the backing owners that are actually retained, so evicting one tile does not necessarily release a tile-sized allocation. Run failure, cancellation and the lifetime of already published data follow ADR 0017.

### 3.5 Semantic descriptors and production progress

A producer's semantic descriptor consists of its resolved Result schema, which fixes the schema id and version, fields, tensor element types, shapes, spatial layout topology and domain, together with global result facts that are fixed once determined, such as field row counts. The descriptor describes structure; Results carry no attached domain label. Resources that operation preparation selects by explicit identity enter the consuming step's physical identity (§3.8), and configuration stays with the step that uses it rather than traveling with a numeric Result. Object identity, publication revision, current coverage and the sealed flag record production progress. Publishing another tile changes progress only and dirties nothing. A change to the semantic descriptor dirties every record that names the producer. A shape or layout change that no longer fits the plan requires preparation and planning again; tile indices from the old plan are not reused.

### 3.6 Record lifecycle

- A successful recomputation replaces the unit's previous record and updates the reverse index, so a producer tile that is no longer read stops propagating dirty to it.
- A failed recomputation leaves the previous generation's tile and record unchanged.
- A task from an older binding generation cannot install its record into a newer generation.
- A completed-tile hit, checkpoint hit or block hit restores the dependencies of the computation it skips. A checkpoint stores the tile and Whole dependencies of the prefix it summarizes; a checkpoint that cannot reconstruct them binds the complete identities of the inputs it depends on. Block keys keep their existing constraints on input state and content.

### 3.7 Dirty propagation

The coordinator keeps a reverse index from each producer unit to the records that name it. A dependency with `whole == true` matches a change to any unit of the producer and a change to its semantic descriptor; a descriptor-only dependency matches a semantic descriptor change. A dirty query starts from the changed workflow-input units, visits each reachable record once, and returns the dirty units per output: tile indices for a Tiled output and unit 0 for a Whole output.

Replacing the binding of a Tiled workflow input compares the new content tile by tile and seeds only the changed tiles; replacing a Whole workflow input seeds its unit 0 when its content changes. A host that implements painting binds the painted layer as a Tiled workflow input, so a stroke dirties only the tiles it touches and their dependents. During dirty propagation, the work is bounded by the records and dependencies reachable from the changed inputs. Binding content comparison and content identity computation are charged separately by the tiles and logical data they actually inspect; both are charged to the Root budgets.

### 3.8 Completed-tile reuse

A computed tile's identity combines the existing physical identity of its step, which includes the tile extent, the resolved output schema, the implementation, the backend profile and the resource identities that affect the result, with the unit index and the identities of every producer unit in its record. Changing the tile extent therefore never reuses an old tile, including tiles of operations without inputs. Input content identity is computed over logical elements and the semantic description; padding, stride, plane order and storage addresses do not participate.

## 4. Non-goals and explicit boundaries

- Sample-level dependency evidence, support roles (Data, Control, Validation, Descriptor) and guarantee levels (Exact, Conservative, Unknown) are not part of the model.
- The smallest unit that is published, cached or reported dirty is a tile of a Tiled output or the complete Whole output. Requests for part of a tile compute the whole tile.
- One square tile extent applies to the whole plan; operations cannot declare their own tile size or tile shape.
- Non-spatial tensors, Results with fields, and Results with several tensors are Whole and accept only whole or descriptor-only Needs; there is no per-member tile request.
- Composite or nested workflows are not kernel nodes; they expand into ordinary nodes before analysis.
- The kernel provides no static region rules such as Elementwise, Halo or Shrink. Operations compute their own Needs; SDK helpers may provide those formulas.
- The coordinator does not split a Whole computation into per-tile calls. PixelOE and other whole-image algorithms keep their current algorithms.
- Records describe one plan and one binding generation; they are not a persistent format.

## 5. Consequences

**Extra computation.** A caller that needs a small region requests every tile it touches and receives complete tiles. A consumer tile that reads a few samples from many producer tiles requests each of those producer tiles, and each is computed completely.

**Wider error domain.** Because a tile is computed completely, an invalid value anywhere in the tile's computation fails the tile and the request that needs it, where the schema or operation requires valid values. For STMap, a non-finite UV value at an unrequested pixel inside a requested tile fails the request. Host schema checks cover every granted producer tile, including samples no tap uses. Tests that require a sparse request inside one tile to succeed around invalid data no longer describe the contract.

**Coarser invalidation.** An edit dirties every consumer tile that requested any part of an edited producer tile, and every tile downstream of those. A consumer of a Whole producer, even one with a spatial layout, or of an input without a tile grid such as a generic STMap map, is dirtied by any change to that producer. After edits the Run recomputes more than an exact sample relation would require, and completed-tile reuse misses correspondingly more often.

**Record memory.** The cost of a record grows with the number of producer units it names. A dense bitset over the producer's whole index space costs $\lceil P/64 \rceil$ words regardless of how many tiles are set: for 100 frames of 7680-by-4320 with 128-pixel tiles, `P = 204000` and each such bitset takes 25,504 bytes, which reaches about 4.85 GiB for one dependency per consumer tile. Implementations therefore start with sorted, deduplicated tile index lists and charge their allocation, copying and scanning to the Root work and metadata budgets; exceeding them returns `ResourceExhausted`.

**Tile size trade-off.** Smaller tiles increase the number of tasks, records and scheduling operations; larger tiles increase wasted computation per request and dirty-set size per edit. The current default extent of 128 remains until measurements justify a change.

**Removed mechanisms.** The sample relation and its constructors, packed Gather evidence, per-sample dependency certificates, support roles and guarantee levels, exact-set normalization used as dependency evidence, tuple closure as dependency evidence, query-scoped dependency bundles and their rebinding, and request-keyed actors are removed. Structural and channel consistency checks that operations still need remain as schema validation. Multi-stage Needs, the static input projection, Root resource admission, cancellation and callback retirement remain.

**Binary interface.** Needs carry tile index sets, a whole-producer request or a descriptor-only request; publication carries no relation. The operation C interface changes accordingly; its version is settled together with the reduced interface described by ADR 0022.
