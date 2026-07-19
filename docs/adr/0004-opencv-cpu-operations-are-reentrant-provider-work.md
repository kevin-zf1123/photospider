# ADR 0004: OpenCV CPU Operations Are Reentrant Provider Work

## Status

Accepted and implemented for repository-owned CPU operation providers.

## Context

Operation callbacks are copied from the process-wide registry and may be
invoked concurrently by scheduler workers, reader snapshots, or the legacy HP
compatibility bridge. Registry locks protect ownership mutation, publication,
snapshot capture, and unload; they do not serialize callback execution. The
operation plugin contract therefore requires each provider to make its callback
reentrant or synchronize its own shared mutable state.

The built-in OpenCV provider previously contradicted that contract in practice.
One process-scope mutex serialized 13 monolithic and tiled callback entry
points across all Graphs and HP/RT routes, even though those callbacks used
immutable input views, callback-local matrices, and independently owned output
storage. Scheduler worker grants were observable at task dispatch but could not
produce operation-level overlap for this set.

Residual `cv::UMat` paths and registration-thread calls to
`cv::ocl::setUseOpenCL(false)` also obscured backend ownership. In OpenCV 4.12,
that OpenCL switch is stored in thread-local core state, so setting it on the
registration thread cannot define scheduler-worker behavior. By contrast,
`cv::setNumThreads` controls process-wide OpenCV CPU parallelism and must not be
reconfigured while OpenCV work is active.

The CLI benchmark service had a separate evidence defect: it reported
`execution.threads` without applying the request to the Host scheduler that
executed the benchmark Graph.

## Decision

Repository-owned CPU OpenCV operation callbacks are reentrant provider work.
The shared outer operation mutex is removed. Built-in resize, crop, extract,
and all repository-owned standard CPU operation plugins use `cv::Mat`, not
`cv::UMat`. Inputs remain immutable; mutable matrices and output regions are
callback-local or task-owned.

The optional repository OpenCV operation provider calls
`cv::setNumThreads(1)` exactly once, before any of its callbacks are published.
This disables nested OpenCV CPU parallelism and leaves the admitted scheduler
worker grant as the repository-owned outer parallelism layer. Repository code
does not call `cv::ocl::setUseOpenCL(false)` and does not reconfigure OpenCV
threading while callbacks can be active.

The same provider owns its OpenCV algorithm implementations and exception
fence. OpenCV resource exhaustion is rethrown as a fresh `std::bad_alloc`;
every other `cv::Exception` is translated to a host-owned `GraphError` with
`GraphErrc::ComputeError`. The provider is included only when
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` is enabled. Dependency-neutral
core operations and the v2 registrar remain available when it is disabled, and
a v2 provider can replace the same registry slots when it is enabled.

Synchronization remains provider-local when a provider owns real shared
mutable state. The Metal Perlin provider therefore retains its DSO-private
mutex around its shared Metal device, command queue, pipeline, and buffer
lifecycle. That lock is not an OpenCV operation lock, scheduler exclusivity
flag, or cross-provider contract.

No scheduler `exclusive` metadata and no public operation/plugin ABI are added.
Third-party providers remain responsible for their own reentrancy and backend
state.

`BenchmarkService::Run` validates the public zero-through-eight worker request
and resolves it exactly once. Before Graph load, it passes that same nonzero
grant unchanged to both future HP and RT Host defaults and reports the
identical value; the zero automatic-selection sentinel does not cross the Host
boundary for later scheduler resolution. A CTest-registered Host-boundary
regression verifies this identity, and callback regressions prove exact overlap
for `1/2/4/8` grants without elapsed-time assertions. A separate manual
benchmark exercises the same Host, scheduler, Graph, and operation path and
reports raw timing samples; its performance ratios are observations, not
correctness gates.

## Consequences

- Independent repository-owned CPU operation callbacks can overlap up to the
  scheduler's admitted worker grant across tiles, Graphs, and intent routes.
- The process no longer pays a hidden global serialization penalty for the 13
  formerly locked entry points.
- Nested CPU work remains bounded because OpenCV internal threading is fixed at
  one before callback publication.
- Disabling the OpenCV operation provider removes its operation callbacks
  without removing dependency-neutral registry/plugin contracts. This option
  does not yet remove separate OpenCV codec, normalization, adapter, or static
  product link dependencies.
- OpenCV dynamic exception types from registered built-in algorithms no longer
  escape the provider boundary.
- `cv::setNumThreads(1)` still affects other OpenCV users in the same process.
  An embedding override would require a separate process-ownership and ABI
  decision.
- Provider-local locks may still reduce concurrency where real shared backend
  state requires them; those locks must be documented by the provider.
- Scheduler worker accounting does not include third-party internal threads,
  hostile DSOs, OpenCV use outside repository-owned providers, or platform
  runtime workers.
- Deterministic concurrency and output-equality regressions are durable product
  tests. Performance measurements remain manual because machine-dependent
  speedup is not a stable CTest or CI threshold.

## Rejected Alternatives

### Add scheduler exclusivity metadata

Rejected because it would encode one implementation accident into planning,
serialize stateless providers, and still fail to protect direct or third-party
callback invocation.

### Keep the outer mutex

Rejected because it defeats admitted task parallelism and conflicts with the
existing provider reentrancy contract without protecting all OpenCV use.

### Keep `cv::UMat` and set OpenCL state in every callback

Rejected because it would mutate thread-local backend state on the hot path,
blur scheduler/backend ownership, and introduce an implicit execution layer
outside scheduler admission.

### Let OpenCV choose its own internal thread count

Rejected for the current product because scheduler workers would then create
unaccounted nested CPU parallelism. Any future policy change requires an
explicit process-level ownership design.
