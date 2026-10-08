# ADR 0019：让算子运行于配置的原生 GPU

- 状态：Accepted
- 英文权威文档：[ADR 0019](../0019-metal-resident-image-workflows.md)

## 1. 核心摘要（TL;DR）

部分算子可以在已配置的原生 GPU 上运行；算子声明支持时，CPU 执行仍可用。`NativeGpu` 选择放置权限，算子契约定义数值行为，内核拥有设备队列、缓冲区、资源计量和同步完成过程。C operation 接口提供有界的主机服务，不暴露设备句柄。

## 2. 架构心智模型（Mental Model & Intuition）

编译器依据所选模式和复制后的 operation 能力，为每个算子步骤标记 CPU 或 GPU 放置。context 拥有 CPU worker pool；启用且设备可用时，还拥有一个 GPU worker lane。GPU callback 获取有界视图并提交原生命令，只有这些命令完成后才返回。

```text
                  ExecutionPlan
                 /             \
           CPU operation      GPU operation
                |                  |
         CPU worker pool    单个 GPU worker lane
                                  |
                          主机拥有的设备队列
                                  |
                           同步原生命令
                                  |
                         已验证的主机可读输出
                                  |
          +-----------------------+----------------+
          |                                        |
       发布成功                      发布前返回 BackendUnavailable
                                                   |
                                      契约允许时重启 CPU 执行
```

每个构建最多选择一个原生 backend：支持的 Apple 构建使用 Metal，或选择可选 Vulkan。`ExecutionContext` 判断该 backend 是否有可用设备。backend 标签记录放置选择；dispatch 和 submission 诊断记录实际设备工作。

## 3. 契约规约与接口（Formal Contracts & APIs）

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

Result operation callback 使用 operation ABI 2。Native GPU service 在独立的 C header `photospider/plugin/native_gpu_api.h` 中使用 ABI 1；C Result service 与 C++ Result phase 会为当前 invocation 提供该表。包含 native GPU header 不要求包含 operation plugin header。

```c
#include <photospider/plugin/native_gpu_api.h>

static int execute_native(const ps_gpu_service_v1 *gpu,
                          const ps_gpu_dispatch_v1 *commands,
                          uint32_t command_count) {
  if (!gpu || gpu->struct_size != sizeof(*gpu) ||
      gpu->abi_version != PS_GPU_ABI_VERSION_1)
    return PS_GPU_RESULT_FAILURE_V1;
  return gpu->execute(gpu->context, commands, command_count);
}
```

`NativeGpu` 为声明支持的 GPU 实现选择放置。没有 GPU 实现的步骤在存在 CPU 实现时使用 CPU；若所选要求没有可用实现，规划返回 `BackendUnavailable`。该模式不选择数值 profile。operation 参数和 traits 定义可接受的值、精度和回退权限。

主机以不透明的调用级 token 借出缓冲区。token 仅指向主机拥有的有界输入、输出或 scratch 视图。输入保持只读；输出和 scratch 分配计入 execution context 的受控缓冲区预算。插件为 Metal 提供 MSL，为 Vulkan 提供 SPIR-V。主机校验记录并拥有设备及 pipeline 状态；service 指针和 token 仅在 callback 生命周期内有效。`execute` 接受有界命令批次，并在已提交工作排空后返回，包括取消和错误路径。成功的非空 GPU callback 必须报告真实 native dispatch。

context 只有一个 GPU worker lane，service 以同步方式提交命令。完成后的共享主机/设备存储可在访问状态转换后由 CPU 读取，该过程不意味着额外的 device-to-host copy。Result 可以在 context 结束后继续持有原生分配 owner 和预算租约。Native result key 包含执行模式、所选 backend 和原生实现身份。GPU 回退会使该结果及其后代不具备 native result-cache 资格；owner 与复用规则见[缓存模型](../../kernel-architecture/Cache-Model.md)。

对 Result operation callback，仅当 GPU 尝试在发布前返回 `BackendUnavailable`、operation 同时支持 CPU、traits 允许回退，且取消/currentness 仍允许继续时，运行期才会回退。内核先释放失败的 GPU continuation 及其临时所有者，再在 CPU 上重启同一个 observation。已提交设备执行错误会终止 Run。

## 4. 负面清单与边界（Non-Goals & Explicit Boundaries）

- `NativeGpu` 授予放置权限，不保证每个步骤都在设备上运行。
- 内核不保证 CPU、Metal 与 Vulkan 间自动数值等价。各算子定义自己的数值契约。
- 原生 GPU callback 是受信任的进程内代码。记录校验不会隔离 shader，也不能防止恶意代码危害进程。
- 设备句柄、队列所有权和 pipeline 管理归主机负责。插件不保留 service 指针或调用级 token。
- 本契约不保证跨图节点的输出页驻留设备，不提供远程 GPU、自动测量式放置或多设备调度器。

## 5. 后果与代价（Consequences）

Execution context 按原生分配的实际容量计量 GPU 输入副本、输出、scratch 和 staging。工作集无法容纳时，准入返回 `ResourceExhausted`；降低并发度或释放保留的 `Value` 可以降低压力。Pipeline 元数据和驱动分配不计入像素缓冲区预算。

设备不可用时，GPU-only operation 无法运行。允许 CPU 实现的 operation 可在 GPU 于发布前报告不可用时使用 CPU 路径。普通 operation 错误和已提交的设备错误会返回调用方，内核不会将其作为 CPU 工作自动重试。调用方可以查看 fallback 原因和实际 dispatch/submission 计数，区分设备工作与 CPU 执行。

同步 lane 明确了缓冲区寿命和发布顺序，但较长的 GPU callback 会占用 lane，直到其命令排空。取消会停止新准入，并等待已提交命令退出后再释放 callback 资源。Result operation ABI 2 与 native GPU service ABI 1 是独立的 plugin 边界。按 `native_gpu_api.h` 编译的 C module 必须在该 header 变化后重新构建；不提供旧 GPU service alias。
