# ADR 0019: Run Operations on the Configured Native GPU

- Status: Accepted
- Reader mirror: [Chinese](zh/0019-metal-resident-image-workflows.zh.md)

## 1. Core summary

The physical Plan fixes CPU/GPU placement and explicit transfers before execution. Operations own numerical behavior; the kernel owns native buffers, pipelines, submission credits and completion lifetimes. An accepted GPU invocation borrows its inputs, outputs and scratch until terminal completion.

## 2. Execution model

Preparation links each task to an immutable kernel. The Run issues ready CPU tasks to its context's worker pool and ready GPU tasks through asynchronous submission. A shared context limit bounds accepted GPU submissions; terminal completion returns that credit. CallGroups retire in IR order independently of task completion order.

```text
                    PreparedPlan
                   /            \
           ready CPU task     ready GPU task
                 |                  |
          CPU worker pool     context GPU credit
                 |                  |
          direct invocation   prepared native submission
                 |                  |
          synchronous return  device terminal completion
                   \              /
                    completion ingestion
                            |
                  IR-ordered call retirement
```

Each build selects at most one native backend: Metal on supported Apple builds or optional Vulkan. Metal uses command-buffer completion handlers. Vulkan retains independent command, descriptor and fence objects for each submission and uses one device completion worker to drain them. The Run keeps accepted work alive through cancellation and failure.

## 3. Contracts and APIs

Operation modules use `photospider/plugin/operation_module_api.h` (ABI 8). Fixed invocations use `fixed_invocation_api.h` (ABI 8). Kernel preparation may borrow `ps_native_gpu_service_v2` from `native_kernel_api.h`:

```c
#include <photospider/plugin/native_kernel_api.h>

static ps_kernel_status_v8 prepare_native(
    const ps_native_gpu_service_v2* service,
    const ps_native_program_v2* program,
    const ps_kernel_preparation_v8* query,
    ps_prepared_kernel_v8* result) {
  if (!service || service->struct_size != sizeof(*service) ||
      service->abi_version != PS_NATIVE_GPU_ABI_VERSION_2) {
    const ps_kernel_status_v8 unavailable = {
        .code = PS_KERNEL_BACKEND_UNAVAILABLE_V8};
    return unavailable;
  }
  return service->prepare(service->context, program, query, result);
}
```

The example receives a valid empty output record and a program/query supplied by the enclosing preparation callback. A successful call transfers one prepared kernel owner. The service table expires when preparation returns; the kernel retains the device and pipeline independently.

A native program supplies MSL for Metal or SPIR-V for Vulkan, fixed bindings, constants and dispatch dimensions. Preparation validates binding ranges and output coverage against the static task signature. Every output is bound once over its full accessible range. Both native adapters accept Shared views and opaque Device handles. Submission checks each binding's memory domain against the prepared signature and resolves its owner through the device's allocation registry.

Shared allocations expose CPU bytes. Device allocations expose an opaque identity and retain their native resource and capacity lease independently of the device wrapper. Metal uses a separate private heap for each allocation; Vulkan uses unmapped device-local memory. Sealing an allocation after producer completion revokes subsequent writable bindings. Explicit CPU COPY tasks connect Host and Shared storage when required by the Plan. GPU COPY supports every Host/Shared/Device pair and preserves raw sample bytes from signed or broadcast affine layouts. Host endpoints use Shared staging declared in the Plan: one output-sized segment per endpoint. Native completion performs any Host destination copy before reporting task completion.

RET and ordered call exports retain Device tiles in their Results. A Result may contain CPU and Device regions in one member; CPU reads intersecting Device backing return `BackendUnavailable`. `PlanningOptions::input_domains` fixes the domain of each external input, defaulting to Host. A subsequent Run in the same context can bind a Device Result directly; a foreign device owner fails `TypeMismatch`. `DenseHost` inputs require the Host domain. Integer byte offsets locate subviews inside Device buffers while opaque allocation identities stay unchanged.

Submission binds actual device-owned allocations. Rejection transfers no invocation borrow and delivers no completion. Acceptance delivers exactly one terminal completion after all device accesses end and writes become visible. Cancellation requests stop further work but cannot shorten this borrow. A device command failure makes the device unavailable and aborts the Run while other accepted submissions drain. The executor preserves compiled placement when reporting a backend failure.

The Plan owns structural facts; each Run owns its bindings and activation state. The operation owns numerical correctness and the requirement to initialize every output sample. Structural validation does not certify shader arithmetic.

## 4. Boundaries

- Native callbacks and shader code are trusted in-process code; ABI validation provides no sandbox.
- Runtime backend retry, implicit transfers, remote GPU execution and multi-device scheduling are outside this contract.
- CPU, Metal and Vulkan numerical equivalence requires an operation-specific contract and validation.

## 5. Consequences

Block admission reserves the actual native output and scratch capacities before issue. Shared allocations count against Host, Device, Shared and Payload capacity. The private allocation API reserves Device and Payload capacity through its caller's reservation callback; native resources retire before that reservation. Metal private capacity includes heap page rounding. Prepared Vulkan constants retain their own native allocation charge. Host allocation owners, pipeline keys, prepared contexts and submission records use the device's metadata root; opaque driver-internal allocations are not an RSS guarantee.

Insufficient capacity returns `ResourceExhausted`. Releasing retained Results or reducing concurrency can reduce the live working set. Results retain native backing and leases independently of context lifetime. Pipeline cache eviction releases cache ownership while prepared kernels retain the pipelines they use.

Slow device work delays drain and retirement. The completion path must retain every borrowed owner until the device reaches a terminal state, including when cancellation is already requested. Module ABI and native service versions are independent; consumers rebuild against the matching headers. Detailed limits and lifecycle rules are in [Plugin ABI](../kernel-architecture/Plugin-ABI.md) and [Compiler and execution](../kernel-architecture/Compiler-and-Execution.md).
