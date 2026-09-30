# ADR 0019: Run Operations on the Configured Native GPU

- Status: Accepted
- Reader mirror: [Chinese](zh/0019-metal-resident-image-workflows.zh.md)

## 1. Core summary (TL;DR)

Some operations can run on a configured native GPU while CPU execution remains available where an operation declares it. `NativeGpu` selects placement permission; the operation contract defines numerical behavior, and the kernel owns the device queue, buffers, accounting and synchronous completion. The C operation interface exposes bounded host services rather than device handles.

## 2. Mental model and intuition

The compiler labels each operation step for CPU or GPU placement from the selected mode and copied operation capabilities. The context owns a CPU worker pool and, when enabled and available, one GPU worker lane. GPU callbacks acquire bounded views, submit native commands, and return only after those commands complete.

```text
                  ExecutionPlan
                 /             \
        CPU operation        GPU operation
             |                    |
       CPU worker pool     single GPU worker lane
                                  |
                         host-owned device queue
                                  |
                       synchronous native commands
                                  |
                      validated host-visible output
                                  |
       +--------------------------+------------------+
       |                                             |
   publish success                   BackendUnavailable before publish
                                                   |
                                      trait-permitted CPU restart
```

Each build selects at most one native backend: Metal on supported Apple builds or optional Vulkan. `ExecutionContext` determines whether that backend has a usable device. A backend label records placement; dispatch and submission diagnostics record actual device work.

## 3. Formal contracts and APIs

```cpp
enum class ExecutionMode : std::uint32_t {
  CpuExact = 1,
  NativeGpu = 2
};

struct PlanningOptions {
  ExecutionMode execution_mode = ExecutionMode::CpuExact;
};

struct ExecutionContextConfig {
  bool gpu_enabled = false;
  std::uint32_t cpu_workers = 0;
};
```

The installed C operation plugin interface is operation ABI 11. Its output sink provides an invocation-local `ps_gpu_service_v11`; the separately versioned structural planar operation extension is ABI 3.

```c
typedef struct ps_gpu_service_v11 {
  uint32_t struct_size;
  void *context;
  int (*buffer)(void *context, const uint8_t *bytes, uint64_t byte_size,
                uint32_t writable, uint64_t *token);
  int (*execute)(void *context, const ps_gpu_dispatch_v11 *commands,
                 uint32_t command_count);
  int (*release)(void *context, uint64_t token);
  uint32_t backend;
  uint64_t minimum_buffer_offset_alignment;
} ps_gpu_service_v11;
```

`NativeGpu` selects GPU implementations that declare support. A step without a GPU implementation uses its CPU implementation when available; planning fails with `BackendUnavailable` when the selected requirements leave no supported implementation. The mode does not select a numeric profile. Operation parameters and traits define accepted values, precision and fallback permission.

The host lends buffers as opaque invocation-local tokens. Tokens refer only to bounded host-owned input, output or scratch views. Inputs remain read-only; output and scratch allocation use the execution context's controlled-buffer budget. The plugin supplies MSL for Metal or SPIR-V for Vulkan. The host validates records, owns device and pipeline state, and keeps service pointers and tokens valid only for the callback. `execute` accepts a bounded batch and returns after submitted work drains, including cancellation and errors. A successful nonempty GPU callback must report native dispatch work.

The context has one GPU worker lane and the service submits synchronously. Completed shared host/device storage can be read by the CPU after the access transition; this path does not imply an extra device-to-host copy. Values may retain native allocation owners and budget leases after the context retires. Native result keys include execution mode, selected backend and native implementation identity. A GPU fallback marks that result and its descendants ineligible for native result-cache entries; see the [cache model](../kernel-architecture/Cache-Model.md) for owner and reuse rules.

For the generic operation ABI, runtime fallback occurs only when a GPU attempt returns `BackendUnavailable` before publication, the operation also supports CPU, its traits permit fallback, and cancellation/currentness still allow work. The kernel retires the failed GPU continuation and its temporary owners before restarting the same observation on CPU. Submitted device execution errors terminate the Run. The planar GPU extension has a narrower contract: it is Whole-only, must perform native work, and does not use CPU fallback.

## 4. Non-goals and explicit boundaries

- `NativeGpu` grants placement; it does not guarantee that every step runs on a device.
- The kernel does not provide automatic numeric-equivalence guarantees across CPU, Metal and Vulkan. Each operation owns its numerical contract.
- Native GPU callbacks are trusted process code. Record validation does not sandbox shader execution or protect the process from malicious code.
- Device handles, queue ownership and pipeline management remain host-owned. Plugins do not retain service pointers or invocation tokens.
- This contract does not promise device-resident planar output pages across graph nodes, a remote GPU, automatic measured placement or a multi-device scheduler.
- The planar extension does not support staged GPU execution, joint dependency results or CPU fallback.

## 5. Consequences

The execution context accounts for native input copies, outputs, scratch and staging using actual allocation capacity. If the working set cannot fit, admission fails with `ResourceExhausted`; reducing concurrency or releasing retained Values can reduce pressure. Pipeline metadata and driver allocations are outside the pixel-buffer budget.

Device absence prevents GPU-only operations from running. An operation with a permitted CPU implementation may use that path when the GPU reports pre-publication unavailability. Ordinary operation failures and submitted device errors are returned to the caller; the kernel does not retry them as CPU work. Callers can inspect fallback reasons and actual dispatch/submission counters to distinguish device work from CPU execution.

The synchronous lane makes buffer lifetime and publication ordering explicit, but one long GPU callback occupies the lane until its commands drain. Cancellation stops new admission and waits for submitted commands to retire before the callback releases its resources. Operation ABI 11 and planar ABI 3 are the current plugin boundaries; plugins must match these interfaces.
