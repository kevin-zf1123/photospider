# ADR 0017: Execute Regional Work with Bounded CPU Storage

- Status: Accepted
- Implementation boundary (2026-10-20): The tile geometry and bounded-resource intent remain; the continuation, polling, shared-stage and publication-grant execution below is historical. Package 0.39 replaces it with fixed tasks, private SSA intermediates, explicit COPY and full-block admission. See the [physical Plan contract](../kernel-architecture/Compiler-and-Execution.md).
- Reader mirror: [Chinese](zh/0017-cpu-regional-execution-and-storage.zh.md)

## 1. Core summary (TL;DR)

Every Result member has a static tile grid. Tiles are the payload scheduling, authorization, publication, cache and invalidation units. A continuation is the execution entity and may span several polls, request several input tiles and produce several result tiles through a shared stage. Full coverage is an ordinary compact tile selection.

## 2. Mental model and intuition

One Run fixes plan, bindings and generation. Ordinary output actors publish one complete member tile. Prepared shared stages perform global algorithms once per original node/stage in that Run; waiters yield without blocking execution workers. State, wait records, terminal records and backing remain Root-accounted. No active continuation crosses Runs.

## 3. Formal contracts and APIs

`ExecutionContext::execute` returns named Results; `execute_fragments` and demand handles select member-local tile sets. `ExecutionOptions::result_publication` observes certified publications and may retain their owners. There is no Tensor-based streaming sink interface.

The plan-wide tile extent is a positive power of two, default 128. Spatial grids split H/W and cover other cell axes; nonspatial grids split the last two non-atomic cell axes. Explicit batch axes and earlier free cell axes form coordinates. Atomic axes remain complete. See [ADR 0023](0023-track-dependencies-between-fixed-tiles.md).

Tensor provides uniform immutable backing and view mappings. Host allocation defaults to tiled planar with actual edge sizes and sparse directories; a full logical canvas reservation is unnecessary. Owning read windows and restricted write windows expose actual strides. Negative strides, broadcast and external buffers remain valid after structural checks. Dense collection and native packing/transfer are explicit, capacity-admitted and charged.

CPU ordinary callbacks use range service ABI 2; staged coordinators use tile service ABI 2. Eligibility follows callback role and worker grant, not coverage size. Nested submission is forbidden and each service joins work before return. Kernel guards save/restore floating state; operations establish their numerical mode on each executing thread.

## 4. Non-goals and explicit boundaries

No separate whole-result mode, default layout/straight-alpha certification, automatic dense copy, dynamic field extent or live-backing eviction/recomputation is added. Resource limits account controlled capacity and work, not total process RSS. Native modules remain trusted in-process code.

## 5. Consequences

Empty payload requests are legal and do not start production; descriptor-only requests are explicit. Out-of-range tile indices fail. Cancellation stops new admission and drains entered callbacks. Failed tile transactions expose no partial payload or dependency evidence. Global algorithms may require full-output work and capacity for one requested tile; their prepared shared stage makes that cost explicit and prevents per-tile repetition. Returned Results and windows retain backing leases until their final owner retires.
