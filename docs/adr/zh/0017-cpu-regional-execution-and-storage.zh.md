# ADR 0017：使用有界 CPU 存储执行区域工作

- 状态：Accepted

## 1. 核心摘要 (TL;DR)

规划器根据算子 traits 传播请求输出区域，执行上下文使用有界 CPU worker 和内存计账调度工作。密集 `Value` 存储保持不可变且可按区域寻址；结构化图像使用 `PlanarImage` owner，并且只发布已完成写入。调用方可以收集请求的输出，也可以同步消费有序输出 tile。

## 2. 架构心智模型 (Mental Model & Intuition)

```text
请求的输出区域
       |
       v
规划器传播需求，并划分允许拆分的轴
       |
       v
有界准入 -> CPU workers -> 算子 callbacks
       |              /                 \
队列或预算       成功输出            失败/取消/stale
限制导致拒绝       |                    |
                   v                    v
                发布结果             停止准入
                   |                排空已准入工作
              收集 / sink                |
                                    返回失败
```

规划器从每个请求输出反向推导，并记录各输入的需求区域。执行上下文只有在队列和分配限制允许时才准入工作。CPU worker 按计划区域调用 callback，发布不可变结果，并在最后一个 owner 释放时回收存储。streaming sink 提供同步背压，因为执行器完成一次 sink 调用后才交付下一个 tile。

## 3. 契约规约与接口 (Formal Contracts & APIs)

```cpp
struct RegionDimension { std::uint64_t offset; std::uint64_t extent; };
class Region;
class Value;
class PlanarImage;
struct ExecutionPlan;
struct ExecutionBindings;
struct ExecutionOptions;
struct ExecutionContextConfig;

using ExecutionSink =
    std::function<Status(const std::string&, ValueView)>;

class ExecutionContext {
 public:
  [[nodiscard]] Result<ExecutionResult> execute(
      const ExecutionPlan& plan, ExecutionBindings bindings = {},
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
  [[nodiscard]] Result<ExecutionDiagnostics> execute_stream(
      const ExecutionPlan& plan, ExecutionBindings bindings,
      const ExecutionSink& sink,
      const CancellationToken& cancellation = CancellationToken(),
      const ExecutionOptions& options = {});
};
```

这些签名还原当前公开成员；省略外围头文件和其他方法。

`Region` 在 descriptor 轴顺序中保存左闭右开的逻辑区间。坐标描述样本，不编码字节偏移。规划器根据 Whole、elementwise 或 halo 等算子 traits 校验请求并传播需求，再按逻辑图像边界裁剪 halo。只有算子和图像契约允许拆分的轴才会被 tile 化；通道轴属于原子分组时，每个 tile 保留完整通道组。收集的输出只包含 plan 请求的确切覆盖范围，而 descriptor 仍描述完整逻辑值。

`Value` 描述逻辑 descriptor、有效 region、strided 字节布局、facets、资源 owner 和不可变 `CpuStorage`。`Value::view(region)` 返回一个共享 storage owner 且不复制数据的 owning `Value`。`ValueView` 则借用 `Value` 且不持有存储；被引用的 Value 必须比它活得更久。`MutableBuffer` 独占且只能移动；`freeze()` 将其存储转为不可变所有权。任一 owning `Value` 持有该分配时，已发布分配仍保持有效。重排和区域算子只将请求覆盖范围复制到宿主管理的输出存储。

`PlanarImageLayout` 定义 height、width、可选 channel 轴、component group、row pitch，以及 continuous 或 tiled 物理顺序。逻辑轴顺序不代表物理存储交错排列。`PlanarImage` 持有已发布且不可变的样本；写入方通过未发布的 write window 写入，并在完成区域后提交。`PlanarPageBudget` 分别计量图像已 backing 的 page 与最大虚拟地址预留。page owner 及计账 lease 可在 execution context 销毁后继续存活。普通 `execute_stream` 入口接收 Value 区域输出；plan 需要结构化 planar 输出时会返回 `TypeMismatch`，planar 执行使用结构化图像结果路径。

当前 operation plugin 接口使用 operation ABI 11，planar operation 接口使用 ABI 3。普通 C 接口定义在 [`operation_plugin_api.h`](../../../include/photospider/plugin/operation_plugin_api.h)，独立 planar 接口定义在 [`planar_operation_plugin_api.h`](../../../include/photospider/plugin/planar_operation_plugin_api.h)。callback 通过注册表选定版本的 ABI 接收已校验元数据、请求区域和宿主服务。注册表会在调用插件 callback 前拒绝 ABI 不匹配。宿主边界会隔离 callback 异常。

`ExecutionContextConfig` 限制 CPU worker 数量、等待 callback 数量、受控活动字节和可选 cache。普通 callback 在 worker 开始执行时释放等待槽位；`CPU_STAGES` job 会保留准入，直到所有已提交 tile 退出且 job 从队列摘除。因此等待上限已满时，即使 staged job 正在运行，也可能阻止另一项提交。共享 scheduler 队列见[并行执行模型](../../kernel-architecture/Parallel-Execution-Model.md)。`maximum_live_bytes` 计入受控计算 payload，包括区域读取、宿主管理的输出、scratch、中间数据和传输。该限制不是进程 RSS，也不计调用方执行前已拥有的输入存储。分配 payload 前先预留预算，计账 lease 会保留到最后一个 storage owner 释放。

## 4. 负面清单与边界 (Non-Goals & Explicit Boundaries)

- CPU 区域执行为必需能力；GPU backend 是可选且按算子支持。GPU 可用不表示每个算子都能在 GPU 执行。
- 受控字节上限覆盖已纳入计账的 payload 分配，不限制调用方分配、未计账元数据、线程栈或操作系统开销。
- `Region` 描述逻辑覆盖，不保证一般 strided `Value` 在物理内存连续。
- planar 图像的通道分组表示逻辑样本和语义分量，不会将 planar 存储转换为交错字节。
- 执行器不会回滚已被 streaming sink 消费的 tile；streaming 没有持久提交协议。
- 可信进程内 operation 代码不受沙箱保护。分配服务和 ABI 校验用于约束 callback 契约，不限制任意 native 代码。

## 5. 后果与代价 (Consequences)

无效、空或越界的输出请求在规划阶段失败。无法满足队列准入、workspace 预留或输出分配时，callback 可能被拒绝。能够在已有 lease 内完成的工作会等待 worker 容量。取消采用协作方式；有效入口之后，已观察到的取消优先于 stale graph 状态和普通算子失败。算子失败不会发布不完整输出，普通 `execute` 失败时不会返回部分收集结果。

Whole 算子的工作集可能比小输出请求占用更多内存，因为该算子会建立完整物化边界。若算子契约允许，调用方应选择支持区域需求的算子和 tile 几何，配置合理的资源限制，并在不再需要结果时释放其引用。共享存储只计一次；外部继续持有的结果会让其计账 lease 保持有效。

`execute_stream` 按确定的名称与空间顺序交付基于 Value 的输出 tile，每次同步调用一个 sink。sink 返回后，其 `ValueView` 即失效；需要保留数据时，sink 可以复制字节，或调用 `ValueView::retain()` 获取 owning `Value`。被保留的输出会让存储 lease 继续有效，并可能耗尽执行预算。sink 失败、取消或过期状态会停止后续交付，已准入 callback 退出后调用才返回。先前消费的 tile 不会因后续 tile 失败而撤销。内存限制不约束进程 RSS；operation/backend 支持取决于算子注册的 traits 和配置的 backend。
