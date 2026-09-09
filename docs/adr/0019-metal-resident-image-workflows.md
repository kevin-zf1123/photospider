# ADR 0019: Metal Resident Image Workflows

- Status: Accepted
- Date: 2026-09-09
- Acceptance: the maintainer explicitly requested implementation of the complete
  S4 plan, including the decisions below, ordered Issue commits, independent
  review, protected delivery and cleanup.
- Baseline: kernel `0e65eac`, daemon `8816f85`
- Reader mirror: [Chinese](zh/0019-metal-resident-image-workflows.zh.md)

## Research and scope

S4 delivers one native Apple Silicon Metal backend and all eight existing image
operations through built-ins and the independently buildable C operation module.
CPU remains required. Explicit placement precedes measured automatic placement;
#209 and calibration remain S5. No other GPU backend, incremental compiler #203,
GUI, daemon wire extension, remote device or persistent GPU cache is added.

The inspected baseline's GPU lane executes synchronous CPU callbacks, copies
host bytes across backend labels and has no native resource. Result keys reject
GPU ancestry. Its tests establish scheduling, not native computation.

Apple's [storage guidance](https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus)
defines shared CPU/GPU-accessible allocations. Its
[completion contract](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted())
waits for commands and completion handlers. The actual Apple M5 probe compiled
and executed a Metal kernel with fast math disabled: `Float(bitPattern: 2)*0.5`
produced GPU bits 0 versus CPU bits 1, and minimum-normal times 0.5 produced GPU
bits 0 versus CPU bits 0x00400000. A shader declaring double failed compilation.
These are hardware feasibility observations, not product acceptance tests.

## Public versions and identities

Package 0.6, operation ABI 6 and OperationTraits 6 replace 0.5/5. Old operation
tables and package-minor requests are rejected; C++ consumers rebuild. There is
one current interface, without compatibility aliases. C++17, WorkflowDocument
schema 2, provider ABI 1 and daemon IPC v3 remain.

Planning selects CpuExact (default) or MetalFp32. Numeric mode and native
implementation capabilities influence physical plan identity; changed trait
encoding changes semantic identity. Runtime handles, allocation addresses,
timings and device epochs never enter semantic identity. No optimization rule
is added. Native program bytes and compilation options identify implementations.

## Native storage, access and completion

ExecutionContext owns an optional actual device, one queue, pipeline reuse and
one synchronous GPU lane. Metal availability requires supported Apple Silicon
hardware; disabled builds and unavailable devices retain CPU execution.
The lane waits for each submitted command buffer before callback retirement.
At most one native submission executes per context; no asynchronous callback
completion API or unbounded device submission queue is introduced.

CpuStorage means immutable CPU-accessible storage and may own a Metal shared
buffer. Value layout, origin, Region and descriptors remain independent of the
allocation. Published bytes are immediately readable. Native owners and budget
leases can outlive their context through retained Values. Mutable output and
scratch cannot be published, released or reused before command completion.

Actual allocation capacities, including native input copies, output, scratch
and staging, belong to the existing controlled-buffer budget. A shared
allocation is counted once. Admission reserves before allocation, cache
retention remains a sublimit, and impossible working sets fail explicitly.
Driver metadata, pipeline objects and caller-preexisting input memory are not
pixel-buffer allocations and are reported separately where observable.

Physical plans expose upload, operation and host-access boundaries with typed
producer references, checked Region/layout and byte bounds. GPU successors
reuse compatible buffers. Host access to a completed shared buffer is an access
transition, not a fabricated device-to-host copy. Actual copy count/bytes,
dispatch count, native completion time and host validation remain distinguishable.

## Trusted pure C GPU service

ABI 6 retains synchronous callbacks and adds host-owned native services:
invocation-local opaque buffer tokens, bounded input/output/scratch views,
MSL program and entrypoint, buffer bindings, constant bytes and dispatch grid.
The host validates records, owns pipeline compilation and command submission,
and returns only after native completion. Plugins do not create devices or
queues, retain tokens, or expose Objective-C types in the installed SDK.
Native shaders remain trusted process code; validation is not a sandbox.

The C11 rgba32f module and built-ins use this same host service for all eight
operations. CPU-only callbacks cannot advertise successful Metal execution.
The host enforces existing descriptor, numeric, publication and cancellation
rules for both registration paths.

## Numeric modes and fallback

CpuExact retains existing CPU arithmetic and cache semantics. MetalFp32 is
explicitly opt-in and uses safe floating-point compilation without contraction.
Each operation and the named representative chain are checked against an
independent CPU oracle with atol=1e-6 and rtol=1e-5. Arbitrary graph composition
can propagate operation errors; this tolerance is not a graph-size-independent
error theorem or a promise of CPU bit identity.

Gaussian coefficients are generated in host double; GPU accumulation is
compensated Float32. Box shrink uses compensated accumulation and actual edge
sample counts. Circle coverage uses host double geometry and exact row spans;
native color computation preserves outside bits. Unsupported HDR/subnormal or
other numerical cases use a checked CPU path before publication. Malformed
inputs still return their established errors.

Fallback is per operation. A CPU fallback in a MetalFp32 chain consumes its
actual inputs and does not make the complete chain CpuExact. Backend or numeric
rejection can retry only before publication, after all submitted work drains.
A submitted device execution error terminates the Run; device loss invalidates
its residency generation. Cancellation stops new admission, drains submitted
work and rejects publication. Ordinary stale checks and frozen validity remain.

## Residency and S3 integration

Native input copies and successful results may be retained within one context.
Keys cover proven immutable input content, Region, actual implementation,
numeric mode and device generation. Generic unproven sources remain executable
without cross-Run content reuse. Dirty mapping only guides demand; exact
content identity authorizes reuse.

CPU exact and Metal derived entries cannot substitute for each other. An
operation that falls back, and its descendants, do not populate expected Metal
cache entries. GPU ancestry never enters S3 disk persistence. Cache clear,
eviction and device loss retire eligibility without releasing active owners.
Independent subscribers retain S3 cancellation semantics; only successful
validated current results enter completed caches.

## Acceptance and delivery

The standalone installed examples/s4_gpu_workflow exposes resident-chain,
all-operations, cache-edits, preview-export and fallback scenarios. It supports
CPU/Metal, built-in/C-module operations and whole/tiled/ROI requests. Nonempty
native acceptance must observe real dispatches. Checks cover halo/edges,
nondivisible sizes, all eight operations, numeric extremes and exact circle
coverage, reuse, device failure, cancellation/stale, allocation limits and
retained Value lifetime. Runtime counters have deterministic assertions;
hardware time has no passing threshold.

Static/shared installed C/C++ consumers and daemon 0.6 package migration are
required. CI retains its existing required jobs; unavailable native tests
explicitly skip and do not claim hardware success. After individual Issue
commits, an independent comprehensive review precedes required CI/bot repairs,
protected kernel-then-daemon merges, Issue/Project settlement and branch cleanup.

## Superseded clauses

This decision replaces only prior CPU-only storage/native-residency limits,
operation/trait/package target versions, and unconditional exact numerical
equivalence for explicitly opted-in Metal execution. ADR 0015 product ownership,
ADR 0016 CPU input semantics, ADR 0017 Region/budget rules and ADR 0018 snapshot,
cache and frozen-execution ownership otherwise continue to govern. Acceptance
defines a target; Issues record implemented and verified delivery.
