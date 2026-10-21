# ADR 0019：让算子运行于配置的原生 GPU

- 状态：Accepted
- 英文权威文档：[ADR 0019](../0019-metal-resident-image-workflows.md)

## 1. 核心摘要

物理 Plan 在执行前固定 CPU/GPU placement 和显式传输。算子负责数值行为；内核负责原生 buffer、pipeline、submission credit 和完成生命周期。已接受的 GPU invocation 持续借用输入、输出和 scratch，直至终态完成。

## 2. 执行模型

Preparation 将每个 task 链接到不可变 kernel。Run 将就绪 CPU task 交给 context 的 worker pool，将就绪 GPU task 交给异步提交入口。共享的 context 上限约束已接受的 GPU submission，终态完成后归还 credit。CallGroup 按 IR 顺序退休，与 task 的完成顺序分开。

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

每个构建最多选择一个原生 backend：支持的 Apple 构建使用 Metal，或选择可选 Vulkan。Metal 使用 command-buffer completion handler。Vulkan 为每次 submission 独立持有 command、descriptor 和 fence 对象，并使用一个设备完成线程排空它们。取消和失败期间，Run 持续保留已接受的工作。

## 3. 契约与接口

Operation module 使用 `photospider/plugin/operation_module_api.h`（ABI 8）。固定 invocation 使用 `fixed_invocation_api.h`（ABI 8）。Kernel preparation 可以从 `native_kernel_api.h` 借用 `ps_native_gpu_service_v2`：

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

示例接收有效的空输出记录，以及外层 preparation callback 提供的 program/query。成功调用转交一个 prepared kernel owner。服务表在 preparation 返回时失效；kernel 独立持有 device 和 pipeline。

Native program 提供 Metal 的 MSL 或 Vulkan 的 SPIR-V，以及固定 binding、constant 和 dispatch 维度。Preparation 按静态 task 签名校验 binding 范围与输出覆盖。每个输出恰好绑定一次，覆盖完整可访问范围。两个 native adapter 均接受 Shared view 和 opaque Device handle。Submission 按已准备的签名检查每个 binding 的内存域，并通过设备的分配注册表解析 owner。

Shared 分配提供 CPU 字节访问。Device 分配提供 opaque identity，并独立于 device wrapper 持有原生资源和容量租约。Metal 为每次分配使用独立的 private heap，Vulkan 使用未映射的 device-local memory。生产者完成后 seal 分配，会撤销后续 writable binding 的权限。Plan 需要时，通过显式 CPU COPY task 连接 Host 与 Shared 存储。GPU COPY 支持 Host/Shared/Device 的全部域组合，并保持有符号或广播仿射布局中的原始样本字节。Host 端点使用 Plan 声明的 Shared staging，每个端点占用一段与输出字节数相等的空间。Native completion 在报告 task 完成前执行 Host 目标复制。

RET 和有序 call export 通过 Result 持有 Device tile。同一 Result member 可以包含 CPU 与 Device 区域；CPU 读取请求与 Device backing 相交时返回 `BackendUnavailable`。`PlanningOptions::input_domains` 固定每个外部输入的内存域，默认 Host。同一 context 中后续 Run 可以直接绑定 Device Result；foreign device owner 返回 `TypeMismatch`。`DenseHost` 输入必须使用 Host 域。Device buffer 内的子视图通过整数 byte offset 定位，opaque allocation identity 保持不变。

Submission 绑定实际设备拥有的分配。拒绝不转移 invocation 借用，也不交付 completion。接受后，在全部设备访问结束且写入可见时，恰好交付一次终态 completion。取消要求停止后续工作，但不能缩短借用。设备 command 失败会将设备标为不可用并中止 Run，同时排空其他已接受的 submission。Executor 报告 backend failure 时保持已编译的 placement。

Plan 拥有结构事实；每个 Run 拥有 binding 和 activation 状态。算子负责数值正确性，并必须初始化每个输出样本。结构校验不认证 shader 算术。

## 4. 边界

- Native callback 和 shader 是受信任的进程内代码；ABI 校验不提供沙箱。
- 运行期 backend 重试、隐式传输、远程 GPU 执行和多设备调度不属于此契约。
- CPU、Metal 与 Vulkan 的数值等价性需要算子专属契约和验证。

## 5. 后果与代价

Block admission 在发射前预留实际 native output 和 scratch 容量。Shared 分配计入 Host、Device、Shared 与 Payload capacity。Private allocation API 通过调用方的 reservation callback 预留 Device 与 Payload capacity，原生资源先于该 reservation 释放。Metal private capacity 包括 heap 的页对齐。Prepared Vulkan constant 持有独立的 native allocation 计费。Host allocation owner、pipeline key、prepared context 和 submission record 使用设备的 metadata root；不透明的驱动内部分配不构成 RSS 保证。

容量不足时返回 `ResourceExhausted`。释放保留的 Result 或降低并发度可以减少存活工作集。Result 独立于 context 生命周期持有 native backing 和租约。Pipeline cache eviction 释放 cache 所有权，prepared kernel 继续持有正在使用的 pipeline。

较慢的设备工作会延迟 drain 和退休。Completion 路径必须持有全部借用 owner，直至设备达到终态，包括已经请求取消的情况。Module ABI 与 native service 版本相互独立，consumer 必须针对匹配头文件重建。具体上限与生命周期规则见 [Plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)和[编译与执行](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)。
