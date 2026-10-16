# Bounded GPU request discovery

### Scope and ownership

The executor owns the table, drains native work, validates each record, and attaches the resulting typed Needs to the operation's yielded `ResultProgramNeed`. Discovery does not trace arbitrary shader reads. Both the C Result ABI 5 service and the C++ `ResultProgramPhase::discover` entry use this Result path. `Tensor` can provide private backing storage, but it does not expose a separate public dependency-discovery phase.

A C Result operation calls `ps_result_services_v5::discover` from its ordinary `poll` callback. The discovery callback receives only current Need reads, native atlas access, scratch, work, cancellation, and GPU dispatch services. It returns an ordinary status and must submit real native work. The outer operation callback returns `PS_RESULT_NEED_V5` when the receipt contains requests. The host rejects publication while discovered Needs remain unresolved. An empty receipt adds no Need and can be followed by further callback work.

```c
uint64_t receipt = 0;
if (services->discover(services->context, capacity, candidates,
                       compute_requests, user, &receipt))
  return 1;
return PS_RESULT_NEED_V5;
```

The callback stores the receipt handle in operation state when it needs the normalized records during a later poll. It releases that handle after reading the records. `discovery_requests` supports a count query with `output == NULL` and `capacity == 0`, followed by a copy into initialized records. The C handle remains owned by that plugin instance across polls until explicit release or operation destruction. A receipt retains no source payload.

### Data layout and memory

For capacity `K`, the logical table size is:

$$
S = 16 + 32K.
$$

The executor charges `candidates + 2S` work before allocating the zeroed Root table. The table uses Root-accounted native storage, not the operation's workspace allocation. The device's rounded table capacity is admitted separately. The host freezes the table only after the synchronous callback and its submitted GPU work drain, then decodes it. No table or service pointer survives the discovery callback.

### Execution and Need state

The discovery callback is synchronous and GPU-only. It may read tensor samples covered by the current Need, acquire an atlas for that Need, allocate or release scratch, charge work, check cancellation, and submit GPU commands. The host requires at least one actual native dispatch for a successful discovery call.

The executor attaches the discovered Needs to the `ResultProgramNeed` returned by the operation's poll callback. The operation can request more Needs in that same yielded Need. The combined stage entry count is limited by the smaller of 65,536 and the dependency set's `maximum_boxes`.

Use a count-only query when the record count is not known, then provide initialized `struct_size` values and enough capacity for the copy. Receipt metadata is immutable and carries no tensor bytes. Each `candidates` value bounds one table, while work and metadata are charged across discovery calls in the active poll. The actor's discovery-work allowance persists across polls; each call also consumes Run and Root work. Duplicate records still consume emission and normalization work.

### Limits and failure handling

`capacity` is positive and cannot exceed 65,536, `DependencyLimits::maximum_gpu_requests`, or the applicable `maximum_boxes`. A zero `maximum_gpu_requests` disables discovery. `candidates` is positive and bounds every emit attempt, including duplicates and attempts beyond table capacity. The shader must not dispatch more emitter invocations than that bound. The table sets overflow only when an attempted record has no free slot; filling the last slot is valid. The host rejects overflow and also checks attempted count against `candidates`.

Table bytes, rounded native capacity, candidate work, decoding work, and request metadata are charged independently to their Root and dependency bounds. Read and atlas access remain limited to current Needs. Discovery cannot issue another Need, publish output, enter a block callback, or recursively invoke discovery. A native callback that submits no work fails with `OperationFailed`. Submitted dispatches drain before table owners retire.

## Registered workflow and decoder coverage

The current C discovery fixture is [`tests/fixtures/gpu_result_discovery_plugin.c`](../../tests/fixtures/gpu_result_discovery_plugin.c), loaded by [`tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp`](../../tests/integration/kernel/gpu/test_gpu_discovery_workflow.cpp). Its independent expected outputs are `8` and `24`; the Metal path asserts four native dispatches. A Control edit changes the selected Data support, while a frozen execution retains its captured Result. The same integration source exercises malformed records, overflow, missing device, failed publication, cancellation, Root cleanup and retry.

The decoder and normalization source tests are [`tests/unit/kernel/test_result_discovery_services.cpp`](../../tests/unit/kernel/test_result_discovery_services.cpp) and [`tests/unit/kernel/test_result_discovery_decoder.cpp`](../../tests/unit/kernel/test_result_discovery_decoder.cpp). These registrations identify executable coverage and do not report a previous run result.

Discovery must run on the active callback entry thread. In C++, `ResultProgramPhase::consume_work` may charge from worker threads; C discovery services remain entry-thread-only. Work and metadata bounds persist across calls in an active poll, and each call also consumes Run and Root work. Duplicate records still consume emission and normalization work.

The capacity, candidate, Root and dependency bounds are independent. Reads and atlas access remain limited to current Needs. Discovery cannot issue another Need, publish output, enter a block callback or recursively invoke discovery. A native callback that submits no work fails with `OperationFailed`. Submitted dispatches drain before the table owners retire.
The native GPU service is independently versioned as `PS_GPU_ABI_VERSION_1`; it supplies the synchronous buffer and dispatch operations used by discovery. Result discovery itself remains part of Operation Plugin ABI 5. Vulkan support is optional at build time. Native GPU behavior depends on an enabled backend and a usable device; protocol mocks cover validation and failure paths but do not establish native execution.

Each wire record occupies 32 bytes: four uint32 fields `input, kind, tensor_slot, reserved`, then uint64 `begin, end`. Payload kind 0 names a nonempty valid member-local half-open tile interval. Descriptor kind 1 requires zero slot and bounds. Reserved bits must be zero. An empty receipt contains zero records. Native GPU service ABI remains 1; ordinary GPU steps and GPU shared stages both initialize Run-native uploads.
