# Tile Storage, Lifetimes and Admission

Status: Proposed breaking design. This page replaces the executor cache design with unique tile storage. See the [Plan contract](Compiler-and-Execution.md) and [Chinese reader version](zh/Cache-Model.zh.md).

## Data layout and ownership

A dynamic materialized tile has identity `(RunId, ActivationId, TileId)`, one producer, one allocation and one immutable domain. Different simultaneously live materialized tiles have distinct backing allocations. Consumers share a tile's read-only handle. Views and block-argument names for that tile refer to the same value rather than materializing another one.

The baseline separately allocates the current block's output buffers at admission. A slab sliced into several logical output buffers is outside this baseline. Allocators may reuse addresses after the previous allocation's final owner releases it; the Plan never aliases live tiles. Scratch is private to a submission and its lane can be reused after completion.

External Result bindings may use existing valid Tensor backing. Preflight checks names, schemas, coverage, layout and owners without scanning samples. Explicit import COPY tasks materialize the required external tiles into the Plan representation. They belong to the earliest consuming call and participate in its dependency graph. Zero-copy binding is permitted only for separate, compatible immutable tile allocations. No hidden input-copy cache exists.

The Run retains bindings through imports/direct reads. Parameters, prepared resources, pipelines and modules survive accepted submissions. Returned Results retain tile allocations and the allocator/device release context needed after execution context destruction.

## Holds and release

For tile `v`, maintain:

$$
H(v)=P(v)+U(v)+L(v)+R_{\mathrm{call}}(v)+R_{\mathrm{ret}}(v)+X(v).
$$

`P` is its reserved/unsettled producer hold; `U` counts distinct unfinished consumer tasks; `L` counts frame/live-through/terminator obligations; `R_call` counts pending call-retirement exports and `R_ret` counts pending final-RET values; `X` counts external owning handles. In-flight input accesses are covered by consumer holds and output writes by the producer hold. Additional views borrow under those submissions or acquire their own owner.

| Event | Hold transition |
| --- | --- |
| Admission | Establish producer, all future consumer, live-through and export holds before any task starts |
| Task completion | Finish accesses; release producer/input holds and scratch once |
| Skip unsubmitted task | Release future input holds and unused output producer holds |
| Call retirement | Transfer selected export holds to delivery records, or drop unused holds |
| Block edge | Acquire chosen successor/live-through holds before releasing old frame holds |
| RET | Transfer return holds into final named Results |
| External release | Drop the owning reference |

Free storage exactly at `H(v) == 0`. Completion, retirement and release are distinct. An unretired intermediate can be freed after its consumers finish if it has no export/live-through obligation. A retired output stays alive while the host retains it. Abort cleanup drops unneeded live/export holds, but preserves every accepted submission's producer and consumer holds until quiescence.

Call delivery and final RET are independent obligations. A tile selected for both receives two holds in addition to its producer/consumer holds. Successful call retirement transfers only `R_call` to the delivery owner. A successful Run transfers `R_ret` to the final result owner; failure or cancellation drops every pending `R_ret`, including values whose producing calls already retired successfully. Already handed-off call values retain their external owners. Failed calls hand off no values, even when some sibling tasks succeeded.

The [prototype](prototypes/ooo_executor.py) exposes `retirement_exports` and `return_values` as separate selections. Acquired internal obligations live in `retirement_holds` and `return_holds`; retirement transfers into `retired_values`, while successful RET transfers into `results`. These dictionaries model owning result collections, not raw handles whose arbitrary copies add ownership. Release through `release_retired_values()` and `release_results()` independently; either release order is valid, and calling release on an empty collection is harmless. Admission failure acquires neither set of holds. The model retains unfinished export obligations until terminal drain, then discards those not handed off.

At a branch, the frame holds the union of possible successor live values, then drops unchosen holds. A loop's new activation retains incoming values while allocating fresh outputs. Compiler liveness accounts for dominating values used beyond the immediate successor. Host-retained exports remain charged across iterations.

## Block reservation

Admission uses a vector of host payload, device payload, metadata, queue and completion capacities. For each memory domain `d`, a sufficient bound is:

$$
A_d=L_d+\sum_{v\in O_b}\operatorname{allocatedBytes}(v)
       +\sum_q C_q S_{q,d}+M_d.
$$

`L` is already live incoming/exported capacity charged to the budget. `O_b` contains new block outputs and persistent temporary tiles. `C_q` is granted in-flight capacity for backend class `q`; `S_{q,d}` is its maximum admitted submission-unit (task or packet) scratch/command capacity in domain `d`; `M` covers frame state, descriptors, queues and delivery records. Sizes include rounding/alignment. Checked sums/products reject overflow. Each task obtains its whole resource vector atomically. A is the total sufficient capacity; new admission acquires only A minus the already charged L, without charging existing owners twice.

At least one usable lane exists for each backend class used by the block. Completion capacity covers every possible in-flight submission. A completion producer never waits for retirement or an ordinary work-queue slot. All metadata needed for completion, skip and retirement is reserved. Allocate before issuing; allocation failure tears down admission with zero tasks started in that block. Opaque driver allocations can still fail operationally.

Reservation belongs to the block and cannot be revoked by other Runs. Capacity freed by a tile is not lent elsewhere until the block ends, although the physical buffer can be freed at its final use. Remaining exports keep their actual charges across the boundary; unused capacity returns to the context. Host-retained values can cause the next block to fail admission. The executor does not wait indefinitely for external release.

This conservative bound trades memory for a clear progress guarantee. Oldest-ready priority alone cannot make lazy allocation safe. Example: `A -> B`, independent C, all outputs 4 bytes, B/C retained for RET, budget 8. If C and A finish first, both remain held and B cannot allocate. The serial order A,B,C fits in 8 bytes; baseline full-output reservation requires 12 and rejects it. A future lazy policy needs a separate proved protected-capacity rule. Smaller legal scheduling blocks or narrower output selections reduce baseline capacity at the cost of parallelism.

## Capacity cost and measurement

An 8192 by 8192 RGBA Float32 image contains `8192 * 8192 * 4 * 4 = 1073741824` payload bytes, or 1 GiB. At extent 128 it has 4096 tiles of 262144 bytes (256 KiB) each. If each call in one scheduling block fully materializes such an image, its output-only reservation is:

| Fully materialized image values | Output payload reservation | Separate tile allocations |
| ---: | ---: | ---: |
| 1 | 1 GiB | 4096 |
| 4 | 4 GiB | 16384 |
| 8 | 8 GiB | 32768 |
| 16 | 16 GiB | 65536 |

These are layout calculations, excluding incoming/exported values, scratch, metadata, allocation padding and driver overhead. They are not RSS or timing measurements. A temporary tile can be physically freed at its last use while its block still owns the reserved budget. Report reserved and live bytes separately, together with allocation count and admission failure details, as defined in [diagnostics](Parallel-Execution-Model.md#diagnostics-and-checks).

The [2026-10-20 native measurements](Parallel-Execution-Model.md#native-measurements) compare the same four-stage 4 MiB-per-stage chain in one block and four blocks on Metal and Vulkan. Coalesced Shared backing peaked at 16 MiB; splitting peaked at 8 MiB. A 12 MiB Payload limit rejected the coalesced block before any task or backing allocation and admitted the split variant. The measurement section reports timing distributions and separate metadata, COPY and drain results.

Coalescing DAG path blocks increases scheduling freedom and can also increase the admitted working set. Evaluate coalesced and smaller legal blocks on the same workflow, separating capacity rejection from queue delay and retirement delay. The baseline remains full-block reservation; capacity-aware partitioning requires measured benefit and an explicit compiler policy, without introducing live tile aliases or a payload cache hierarchy.

## Identity and cache boundary

IR/Plan identities serve reproducibility and optional host artifact lookup. PreparedPlan reuse shares immutable code/resources. Immutable input snapshot utilities can remain outside the executor and provide binding owners; they do not authorize execution-result reuse.

The replacement executor has no completed-result cache, checkpoint cache, block-state cache, residency mirror, automatic input-copy cache or hot-path tile-content hashing. Each admitted task executes once unless skipped by abort/failure or elided during planning. A host can bind cached outputs as explicit inputs under its own policy.

`COPY host -> device` produces a different tile. Shared coherent memory is one declared domain with one allocation and synchronization. Changing backend requires a different prepared/compiled placement contract, never a hidden alternate allocation for the same tile.

## Limits

The bound covers controlled capacity, not RSS, stacks or opaque driver memory. There is no spill, eviction, recomputation or storage donation. Full-block admission may reject programs executable under a smaller-memory serial schedule. The prototype checks abstract byte budgets and holds, not physical device or operating-system allocation behavior.
