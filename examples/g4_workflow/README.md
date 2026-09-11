# G4 workflow

The current `data` scenario runs a public WorkflowDocument/Compiler/
ExecutionContext identity workflow over a logical billion-element source.
It requests coordinates 1 and 999999998 separately through the existing
regional executor, constructs exact ValueFragments, checks that the middle hole
cannot be read, checks a per-output certificate transpose, and imports a generic
Int64 snapshot. Only two actual source samples are read. This scenario exercises
the data foundation.

The `progressive` scenario registers a C++ staged operation through the public
registry, compiles a billion-element workflow and executes it with one CPU worker
and a 128-byte controlled allocation budget. Control samples 0, 1 and 3 discover
a payload read at 999999999. The source callbacks reject every unexpected read;
the independent expected result is 17.25 and exactly 32 source bytes.
No full input materialization occurs.

The `stmap` scenario uses the built-in sampler with an upstream computed map and
wrap boundary. Its source rejects every read except pixels 0 and 1023. The
`radius` scenario patches a distant Int64 radius through InputSnapshotStore;
scatter changes from 1 to 5, gather stays 1, and the frozen old execution stays 1.
The returned runtime dependency records prove that editing radius[3] potentially
dirties scatter{0} but leaves gather clean within the recorded query. The new
source edge is present after recomputation, while frozen evidence retains the old
relation. Restricting evidence to scatter also removes the gather root.
Operator contracts are in [Dependency Sampling](../../docs/kernel-architecture/Dependency-Sampling.md).

The `demand` scenario opens a context-owned handle and requests `{0,4}` in one
exact sparse query. Two binding replacements change a radius and then its data
without recomputing between edits. Dirty accumulates over both endpoints. The
direct radius predicate gives latest values `[1+9,5+9]`, while a frozen bundle
still returns `[1,5]`. It also rejects the middle hole and releases the exact
subscription. These calls use the existing worker/allocator owners.

The `shared` scenario submits two exact waiters against one immutable bundle.
A bounded barrier in a registered identity callback lets the second waiter join
the actual in-flight observation before the first is cancelled. Exactly one
callback runs; the second waiter still receives 7 and complete identity evidence.
Shared active work is available with result retention disabled.

The `cache` scenario enables the existing result LRU and reuses two exact
observations. An unrelated data edit still hits both. A changed control with
unchanged numeric output recomputes both and installs the new data edge. Clearing
cached pixels leaves that new dirty relation available in the returned evidence.
The independent radius predicate still gives values `[1,5]`.

The `reductions` scenario streams a logical 32 KiB source through `numeric.mean`
and `numeric.variance` under a 1 KiB controlled live budget. Each source callback
requires exactly 64 Float64 samples. Uniform `[0,1,2,3]` repetitions independently
give mean 1.5 and population variance 1.25; one mean pass and two variance passes
read exactly 98304 bytes. Runtime evidence retains the complete global support.
The scalar operations use ordered incoming accumulators, never partial block
sums.

The `scan` scenario computes 128 inclusive prefixes of `[1,2,...,128]`. The
triangular-number oracle checks every output, ending at 8256, while source
callbacks reject any repeated prefix read. Completed carries let all 128 outputs
read exactly 128 inputs. A separate `[1,inf,...]` binding then proves `{0}`
succeeds with 1 and `{0,1}` reports the error at input 1.

The `blocks` scenario edits the first input in `[0,1,2^54,4,5,6]` to 1.
The first three one-sample transitions must recompute; after the accumulator
reconverges at `2^54`, the last three transitions reuse completed states. The
prefix ending at input 1 is now 2. A separate volatile binary64 left fold checks
the final output; runtime evidence still records the complete current source
prefix. Cache keys include incoming state, not just equal outgoing carries.

```sh
cmake --build build/issue257-static --target photospider_g4_workflow -j 8
build/issue257-static/examples/g4_workflow/photospider_g4_workflow
```

Expected checked output:

```text
data: values=[1,999999998], source_reads=2, hole=rejected, transpose={1}, generic_snapshot=ok
progressive: value=17.25, controls=[0,1,3], payload=[999999999], source_bytes=32, budget=128
stmap: red=0.5, source_pixels=[0,1023], source_bytes=32
radius: scatter_before=1, scatter_after=5, gather=1, frozen=1
dependencies: radius[3] -> scatter{0}, gather{}, new_data_edge=present, frozen_data_edge=absent
demand: Q={0,4}, latest=[10,14], frozen=[1,5], generation=3, accumulated_dirty={0,4}, release=ok
shared: callbacks=1, first=Cancelled, second=7, evidence=present
cache: warm_hits=2, unrelated_edit_hits=2, control_edit_hits=0, values=[1,5], cleared_pixels=0, data3_dirty={0,4}
reductions: mean=1.5, variance=1.25, block=64, source_bytes=98304, live_budget=1024, global_support=present
scan: outputs=128, source_reads=128, last=8256, short_query=1, joint_query=nonfinite_input_1
blocks: first_misses=6, edit_misses=3, edit_hits=3, prefix1=2, source_support=all
```

It also builds as a standalone installed public package consumer:

```sh
cmake --install build/issue257-static --prefix "$PWD/out/g4-install-static"
cmake -S examples/g4_workflow -B out/g4-consumer-static \
  -DCMAKE_PREFIX_PATH="$PWD/out/g4-install-static"
cmake --build out/g4-consumer-static -j 8
out/g4-consumer-static/photospider_g4_workflow
```

Optional single-pixel cost measurement (not a timing gate):

```sh
build/issue257-static/examples/g4_workflow/photospider_g4_workflow --measure
```

This runs 256 RGBA pixels through `image.stmap`, one worker, with result caching
and cross-pixel batching disabled. Each pixel invokes three poll phases. It
checks all output bytes against the source and requires exactly 256 certificate
rows and 768 poll calls. After one warmup, it reports the median end-to-end
`execute` duration of three Runs and the poll duration from that same Run.
Poll timing excludes queue waits and structural publication; it includes the
host session's poll validation. End-to-end timing includes the public execute
path, exact-set/certificate work and result assembly. Oracle checking is outside
the measured interval.

Observed on 2026-09-11, Apple M5, RelWithDebInfo static kernel with runtime evidence:
`median_execute_us=74172`, `poll_callback_us=6083`,
`execute_us_per_pixel=289.734`. These measurements describe this workload and
host/load, not a general performance promise. Most measured time is outside poll
callbacks; no `PerAtomOutcome` batching has been enabled based on these numbers.
