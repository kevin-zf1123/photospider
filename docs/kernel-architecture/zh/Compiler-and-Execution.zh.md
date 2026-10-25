# Compiler、Plan 与 Executor

状态：Proposed breaking design。本页与[并行执行](Parallel-Execution-Model.zh.md)、[tile 存储](Cache-Model.zh.md)、[依赖](Dependency-Data.zh.md)替换执行设计。下列表描述目标契约。C++ 公共 API 通过 `prepare`、`submit` 和 `join` 执行物理 CPU 程序及 Host 域异步 GPU 入口。Native Shared 存储支持 CPU 映射访问和显式 Host/Shared COPY task。Metal MSL 和 Vulkan SPIR-V kernel 在 preparation 时准备一次，并在原生设备上异步完成。Device 私有分配支持 native kernel、有序 Result publication 和显式 Device 输入契约。显式 GPU COPY task 以逐字节精确的仿射读取连接 Host、Shared 与 Device 存储。[参考模型](../prototypes/ooo_executor.py)独立验证新的调度语义。[英文权威文档](../Compiler-and-Execution.md)。

## 范围与职责

Compiler 生成完整物理程序。Executor 把就绪 task 发射到 CPU worker 和 GPU queue，接收完成事件，并按 IR 顺序退休 call。算子采用 unchecked 执行契约：调用方提供合法数值输入，实现遵守声明的访问范围、数值语义与完成义务。

| 所有者 | 职责 |
| --- | --- |
| Frontend 与 IR validator | 源语义、SSA、支配关系、固定 schema、控制流和 call 退休顺序 |
| Planner 与 Plan validator | Task 展开、固定读取、唯一 tile producer、放置、传输、资源上界、依赖与 call 归属 |
| Loader 与 prepare | 有界解析、契约/ABI 链接、具体设备兼容性与不可变 kernel 绑定 |
| Run coordinator | 就绪、准入、完成、退休、取消与所有权 |
| CPU/GPU 实现 | 数值行为、声明的读写、有界 scratch、包含可见性保证的真实完成 |
| Host | 每个 Run 的不可变 bindings、结果消费和外部编排 |

Kernel 保持 [ADR 0015](../../adr/zh/0015-breaking-product-boundary-scope-reset.zh.md) 的组件边界。Daemon IPC 和临时 job 留在外部。结构校验在发射前完成。执行期间检查操作性失败和自身状态，不扫描样本、不调用数值 validator、不认证输出内容，也不通过 `Need` 重新发现依赖。

## 程序阶段与语义顺序

```text
WorkflowDocument / IR builder
              |
         ValidatedIR -- optional named optimizers --> ValidatedIR
              |                                           |
              +---------------- named planner -------------+
                                    |
                               ExecutionPlan <--- checked .psxp import
                                    |
                              prepare(device)
                                    |
                               PreparedPlan
                                    |
                         Run + immutable bindings
                            /               \
                       CPU pool          GPU queues
                            \               /
                             completion events
                                    |
                            ordered retirement
```

`FrontendRegistry`、`OptimizerRegistry`、`PlannerRegistry` 保持具名注册和冻结后的共享所有权。规划独立于可选优化。Planner 通过私有 builder 构造 task/tile/call/block 表，由 compiler 校验完整结果后发布，替换只能修改指令 vector 的 builder。未知名称返回 `NotFound`，非法生成程序返回 `Internal`，格式错误的导入程序返回 `InvalidArgument`，不兼容契约返回 `TypeMismatch`；验证 work 或分配耗尽保持 `ResourceExhausted`。具名 planner 的 callback 失败、异常和非法生成程序会在诊断中标明 planner 名称与版本。

`IRProgram::calls` 是声明表。每个 ControlFlow block 的 call 列表定义退休序号。DependencyPaths 去重后，同一 Call ID 的重复请求只形成一个调用位置；不同 Call ID 仍是不同调用。连续、无参数、通过无条件单前驱/单后继边连接的路径 block 合并为一个调度 block，保持首次请求顺序。人为的 DAG 路径边界不得串行化独立工作。分支、循环头和合流保持显式边界。

Block 内的数据使用指向更早的 call、传入值，或同一 call 内无环任务图中的 producer。Task 不依赖更晚 call 的输出。物理 task 表顺序独立于 call 顺序。纯 task 可乱序发射和完成；退休保持动态 call 序列的连续前缀。Tile 成功完成后立即允许内部消费者运行，即使其 call 尚未退休。

每个保留的 IR call 都有 `CallDesc`，包括没有选中 task 的纯 call。零 task call 以 `Elided` 退休，不携带计算值。输出选择因此保持 call 记录顺序，同时不声称被省略的工作已经执行。Effect 和必需控制依赖始终保留。

## 物理 plan

以下 C++17 声明描述逻辑记录，不是直接序列化的内存格式。ID 索引不可变 Plan 表，`Slice` 索引指定的辅助数组。零是合法索引，`none` 为保留值。

```cpp
#include <cstdint>
#include <limits>
namespace ps_plan_design {
using Id = std::uint32_t;
constexpr Id none = std::numeric_limits<Id>::max();
struct Slice { Id offset, count; };
enum class TaskKind : std::uint8_t { Kernel, Copy, Effect };
enum class EndKind : std::uint8_t { Jump, Branch, Return };
struct TileDesc {
  Id schema, member, layout, domain;
  std::uint64_t tile_index, bytes, alignment;
  Id producer;                    // none for an incoming immutable value
};
struct ReadDesc { Id port, tile, view; };
struct TaskDesc {
  TaskKind kind;
  Id call, kernel, placement;
  Slice reads, writes, schema_reads, parameters;
  Slice predecessors, successors;
  Id scratch;                     // per-domain resource vector
};
struct CallDesc {
  std::uint64_t source_call;
  Id ordinal;
  Slice tasks, returns;            // task IDs and selected result descriptors
  Id effect_task;                 // none or one terminal effect task
};
struct BlockDesc {
  Slice tasks, calls, arguments, live_ins;
  EndKind end;
  Id condition;                   // host-readable UInt8 scalar, or none
  Slice edges, return_values;
  Id reservation;
};
struct EdgeDesc {
  Id target_block;
  Slice arguments, live_values;   // value IDs captured simultaneously
};
}  // namespace ps_plan_design
```

辅助表保存 schema、成员 grid、layout、参数字节、不可变资源 identity、kernel symbol、placement 要求、资源向量和 ID 列表。`ValueDesc` 把 IR Result 与 schema 及有序 `(member, tile index, TileId)` 条目关联；边参数和 return 引用这些值。Read view 记录已校验的逻辑范围、字节范围和 stride。Schema-only read 引用静态 metadata，不产生 payload 依赖。

一个 task 可以产生多个独立 tile，包含同一 call 的多个输出。这些 tile 在该 task 成功完成时一起可用。静态共享 stage 变为带私有 SSA tile 输出和显式消费者的普通 task，每次 call activation 实例化一次。持久临时数据属于 SSA tile，任务局部临时数据属于 scratch。隐藏分配不在有界算子契约内。

每个物化输出 tile 有一个 producer 和独立 allocation。Fan-out 共享该 tile 的不可变 handle。`Copy` 在指定内存域产生新 tile。Block 参数为已有不可变值赋予新名称，不物化第二个 tile。多 tile Result 是 owning handle 集合，不自动组装稠密 backing。

GPU COPY 读取一个仿射来源并写入一个 dense 目标，在 Host、Shared 与 Device 的全部域组合之间保持样本字节。来源支持有符号及零 stride、未对齐字节原点和奇数字节数。Metal 使用按字节寻址的 compute kernel；Vulkan 合并连续的末尾维度，将其余区域编码为固定批量的 buffer-copy command。

目标包含 `B` bytes 时，GPU COPY 为每个 Host 端点声明 `B` bytes Shared scratch。因此 Host-to-Host GPU COPY 需要两段互不重叠、总计 `2B` 的暂存空间，Shared/Device 端点无需 staging。`OperationProgramBuilder::finish` 在编译时推导此声明；自定义物理程序与导入 artifact 必须已经声明足够 scratch。Preparation 保持 task 图与 scratch 声明不变。Block admission 为每个 GPU lane 分别预留 native staging capacity。Host 来源在原生提交前完成 gather；Host 目标在原生成功后、task 终态完成前从 staging 复制。取消不能提前释放已接受工作。

外部分配契约在编译前按声明名称选择：

```cpp
ps::PlanningOptions options;
options.input_layouts["image"] = ps::physical::InputLayout::DenseHost;
```

省略的声明使用 `DirectTiles`，借用独立且兼容的 tile allocation。`DenseHost` 描述完整 tensor member 的 row-major Host 存储，包含 batch 轴。原始 tile view 共享 member 的 stride 和容量，偏移由各 tile 的逻辑原点计算。`submit` 相对于绑定基址检查这些 view；所属存储允许在该范围前后保留未使用字节。来源 owner 保持不可变，并保留到最后一次读取结束。对于 extent 为 2 的 UInt8 `[4,4]` member，原始 tile 的 stride 为 `[4,1]`，偏移为 `0,2,8,10`，容量为 16。四个 COPY 产生独立的 4 字节 tile，stride 为 `[2,1]`。

Planner 为每个使用到的 `(block, input)` 建立一个 `Imported` value，为每个选定 tile 建立一个 COPY，同一 block 内的消费者共享该导入结果。`import_source` 标识规范 Input，`owner` 标识 block，`source_value` 为零。Validator 证明每个 import 从该输入复制相同 member、tile index、类型和逻辑范围。Payload 消费者与终结符使用 imported value；schema-only read 仍可引用原始输入。边参数将 imported handle 重命名为目标 block 的 dense argument。每次 block activation 创建新的 import allocation。

服务于算子的 COPY 归属该 block 内最早的实际消费 call。`select_outputs` 删除其他 task 时，保留已保存的 call 归属。仅供 condition、边参数或 RET 使用的输入需要 `call = none` 的边界 COPY。边界 COPY 在所有真实 call 成功退休后发射，遵循相同的分配、完成和 hold 协议，不产生 CallRecord。这也覆盖零 call 的纯输入程序。其分配计入完整 block reservation。原始 view、import 来源与 COPY task 都参与序列化和 Plan identity；prepare 与 submit 不改变拓扑。

`OperationRegistry::prepare_operation` 在调用静态回调前复制输入 schema，并按值保存静态参数，保留浮点位模式。静态回调、lowering 和 kernel preparation 共享捕获的 schema。Preparation 返回后，调用方可以修改原始 schema 和参数 map；这些修改需要新的 preparation 才能影响执行。调用方修改不得与 preparation 并发。输出选择与并发 Run 保留同一份不可变静态 state。即使 registry 和外部 preparation handle 已销毁，prepared kernel 仍将该 state 保留到自身 release 回调结束。

`OperationDefinition::validate_resources(state, resources)` 在静态特化和资源选择后，对每个 `PreparedOperation` 校验一次所选不可变资源。它在 preparation 发布前运行，覆盖 Empty call 和 artifact 重新绑定。回调借用不可变静态 state 及已声明的冻结资源，不读取 sample payload 或执行 I/O。失败会拒绝 preparation 并释放临时 owner。Lowering 和逐 task preparation 保留已接受的资源 owner，不重复该检查。

`prepare_kernel` 接收 symbol、静态算子 metadata、完整 task view、task 参数字节、scratch 上界、资源和设备。它检查 symbol 的调用契约，并生成包含直接入口、资源 owner 和 pipeline 的不可变 `PreparedKernel`。PreparedPlan 保留这些 context 和模块；invocation 借用其 context 直到完成。所有引用 Run 排空后才能销毁 context。链接按已保存的 `KernelDesc::abi_version` 选择 factory：版本 8 使用 `prepare_kernel`，可信 C++ CPU 版本 9 使用 `prepare_kernel_v9`。没有对应 factory 时拒绝，不降级 ABI，也不重跑 lowering。FRQ-01..06 要求 typed CPU 9，旧 FRQ 8 Plan 需要重新编译。Module ABI 8、native service ABI 2 及 Plan v5 序列化布局保持不变。

`Placement` 固定 backend 类别、数值 profile、内存域和 queue 能力。`prepare` 将其绑定到具体设备对象和 kernel，可以拒绝不可用 backend，但不得改变 task 拓扑、实现 profile 或 COPY 边。`CpuExact` 要求所有 task 为 CPU。静态 planner 的 `NativeGpu` 对支持 GPU 的 operation 请求 GPU lowering，对 CPU-only operation 请求 CPU lowering。当 operation 同时声明两种能力时，其 GPU lowering 可在同一 call 内显式组合 CPU/GPU task；每个非 COPY task 都按 traits 检查。COPY placement 遵循声明的 domain。一个 Plan 因而可以包含两种 backend，不需要运行时选择。同一 operation 的 CPU/GPU 替代方案需要分别编译 Plan；preparation 与执行期间没有隐式重试或 placement fallback。

## 具体调度 block

以下汇编记法用于解释；语义原型直接构造等价 Task record，不解析 `.psxp`。耗时是模型输入，不是 Plan 调度保证。

```text
block entry:
  retire [call A, call C, call B, call D, call E]
  tile a, b, c, d, e: u8[4], host, separate-allocation
  task t0: call A, kernel @A, cpu, reads [],     writes [a], deps []
  task t1: call C, kernel @C, cpu, reads [a],    writes [c], deps [t0]
  task t2: call B, kernel @B, cpu, reads [],     writes [b], deps []
  task t3: call D, kernel @D, cpu, reads [b],    writes [d], deps [t2]
  task t4: call E, kernel @E, cpu, reads [c,d],  writes [e], deps [t1,t3]
  ret result = [e]
```

Compiler 生成 successor `t0:[t1], t1:[t4], t2:[t3], t3:[t4], t4:[]`，call 成员数为 `[1,1,1,1,1]`。两个 CPU lane 时，t0 与 t2 一起发射，不会仅为保持退休列表而增加 t1 到 t2 的依赖。GPU 版本在 prepare 前插入独立 host-to-device/device-to-host COPY tile/task 并确定 placement；执行期间不推导这些复制。

成员 tile 几何保持一个正的 2 的幂 plan extent，默认 128，以及 ADR 0023 的 spatial/batch/atomic-axis 规则。Grid 几何确定逻辑 tile，独立物理 allocation 确定存储。一个 128 × 128 像素的 RGBA Float32 tile 含 262144 bytes，与 call/task 编号无关。

## 静态构造与校验

Planner 从选定 return、条件、边参数、跨 block 存活值和 mandatory effect 对读取求闭包，只在编译期调用纯静态 mapper。数据相关采样声明有限保守超集；任意 gather 可能需要全部来源 tile。Runtime 不扩大依赖集合。

发布 Plan 前，validator 建立以下结构性质：

1. 索引、slice、大小、对齐合法，算术经过溢出检查，解析和规划工作量有界；schema 兼容，SSA 支配关系与 CFG 合法。
2. 每个物化输出 tile 恰好一个 producer，layout/domain 相容，read view 合法；输入来自局部 producer 或声明的传入值。
3. Block task 图无环。Predecessor 为去重后的 producer ID，successor 为其精确转置，初始 pending 等于 predecessor 数量。
4. 每个 task 恰好属于一个 block。算子 task 及其 import COPY 属于一个真实 call；终结符的输入 import COPY 使用 `call = none`。Call ordinal 连续，producer call ordinal 不大于 consumer call ordinal；真实 call 不能依赖边界 COPY。Task 数组位置不约束执行顺序。
5. 每个 effectful call 在全部纯 preparation task 后有一个终端 effect task。它不再指向本 call 的 preparation，并且只在退休头获得执行资格。
6. 执行路径产生必需条件、return、边参数和支配的跨 block 存活值。只为选中的后继建立动态 activation。
7. 资源向量覆盖输出、持久状态、descriptor、command metadata、队列/完成记录、交付记录和 scratch。Backend 支持声明的同步。

这些检查校验声明，不证明任意 native 实现。算子负责在报告成功前写完全部声明输出，并遵守数值语义。未声明访问或非法数值前置条件违反 unchecked 契约，不保证产生诊断。

`OperationProgramBuilder` 在追加条目前检查各表记录总量和已构造引用总量。共享 tile 只计一次，每个实际 task read 单独计数。Finalization 使用相同限制检查派生的 predecessor、successor 和 live-in。Task 参数字节继续由最终 work 校验约束。

`select_outputs` 在已有表上求子集闭包，保持 control/effect 和 call 顺序，重建 task 归属、邻接表、return hold 和资源上界。它不调用 mapper，也不增加缺失 tile。超出已编译覆盖的请求需要重新编译。空选择仍执行 control 和 effect。

## Run、控制流与返回接口

`ExecutionPlan` 的副本共享可序列化的不可变程序；输出选择在修改前复制私有存储。`PreparedPlan` 保留该程序对象，并持有已链接 kernel、设备对象和模块/资源 lease。准备阶段向 context 资源根准入新增表、query/bridge metadata 和 kernel 包装对象。`RunHandle` 持有可变状态与不可变 binding lease，并保留 prepared program 直至 drain。并发 Run 共享 prepared code 和 context pool，输出分配相互独立。

逻辑 API 为 `prepare(plan, device_context)`、`submit(prepared, bindings, options)`、`request_cancel(run)` 和 `join(run)`。可能失败的操作返回类型化 status。`join` 等待全部已接受提交停止访问资源，返回完整具名 Result 集合或一个终态错误。复制终态错误文本时若内存耗尽，`join` 保留错误码、reason 和结构化 detail，并返回空 message。Result 通过保留 allocation release context，可以比 Run 和 context 活得更久。

可选 `on_call_retired(CallRecord)` 接收 sequence、activation、来源 call、status 和选定导出值。健康 observer 为每个退休 call 接收一条记录，包括 elided call。不导出的中间值没有交付 hold。Block admission 为有界 delivery queue 预留存储；用户 callback 在 coordinator 外执行，与完成事件处理相互独立。

每个 Run 有一条不可重入的串行 delivery lane。Callback n 正常返回后才启动 n+1，因此调用开始与正常返回的顺序都遵循退休顺序。不同 Run 可以并发交付。`join` 等待 task drain 和交付结算；callback 不得同步 join 自身 Run。慢 callback 可以延迟 join，或占用下一 block 所需容量，但不能阻止 CPU/GPU 完成事件处理。

Callback 抛异常时立即关闭该 Run 的 delivery lane：不再启动后续 callback，丢弃已排队记录并释放其 delivery hold，用户独立保留的值继续有效。Lane 关闭与 enqueue 串行化。关闭后的 enqueue 拒绝记录且不接管 owner；coordinator 释放对应 delivery hold 并将记录标为 settled，包括与失败通知竞争的退休。Delivery lane 发布一条预留 settlement event，原子携带 `OperationFailed`、observer origin 和对应 CallSeq。失败 callback 的记录在 coordinator 消费该事件前保持 unsettled；coordinator 在同一次状态转换中先记录 abort，再将该记录标为 settled。禁止先报告成功 acknowledgment，再单独发送错误事件。Coordinator 停止后续准入与退休，并排空已接受工作。Callback 失败前，退休可能已经允许更晚的 effect 执行；这些 effect 无法撤回。

最终 RET 数据保持内部持有，直到全部已接受工作停止访问，且 coordinator 确认所有退休记录已经完成交付结算，包括最后一个 callback。未设置 observer 时，记录立即结算。Coordinator 在一个串行化 finalization 点处理待决 delivery/stop 事件并锁定终态；仅成功时才把最终 RET owner 转交调用方。因此，即使全部 kernel 已完成，最后一个 callback 的异常仍会阻止成功 RET。在该点之前观察到的取消保持已定义优先级；该点之后的请求不能改变终态。原型直接模拟拥有型退休记录，尚未实现此 callback 协议。

进入 block 时为新单调 activation 建立全部 task/call 状态。成功 terminator 在 block 的 call 全部退休且 task 全部完成后运行。Coordinator 读取 host 可见条件，同时捕获所选边的全部参数和跨 block 存活值，取得下一 frame 的 hold，再释放旧 frame。Compiler liveness 包含跨越直接后继使用的支配值，即使它们不是显式 block 参数。未选后继不实例化。

循环每轮创建新的物化 tile identity。传入值保留旧 allocation，直到最后一次读取结束。每次 block entry 都消耗有界 work，包括空循环；activation/sequence 溢出时拒绝继续。Device 中的分支标量需要显式 host COPY。Coordinator 在安全点检查取消与当前性。

Typed invocation 9 数值 checkpoint 通过借用的 Run 服务预付 work/stage。Coordinator 与 kernel 共用先 Run 后 Context root 的两维原子准入；`issued_work` 记录成功准入量，不表示 CPU 指令或 legacy v8 内部算术。真实 host 预算拒绝保存 `ResourceExhausted`、`WorkLimit`/`StageLimit`、`Resource`/`Run` 及已知 node/task。Coordinator 观察该状态后中止调度并排空已接受工作，即使拒绝的 kernel 随后报告 OK 也不能成功。逐 lane 安全点限制后续 issue；竞争时已经接受的 invocation 保留 owner 至返回。取消保持终态优先级，已准入费用不退还。算子每 invocation 的 `max_work` 失败保持普通 kernel failure，不作为 host Run 预算拒绝。

每个动态产生的 Result 在 block admission 时、kernel 发射前预约一个进程内唯一的 ObjectId。只能移动的预约 token 与 `ResultBuilder` 使用相同分配器，且只能构造一个规范 Result 对象。该 value 的所有 tile 和 member 通过 `ps_kernel_invocation_v8::input_object_ids` 暴露相同 ID，数组顺序与 input view 一致。新的循环 activation 与独立 Run 预约新 ID；CFG 参数和跨 block 存活别名保留来源关联。ID 属于运行期 metadata，不进入 Plan 序列化或内容 digest。

规范 Result 包含原始动态 value 的完整已编译 tile 集合。完整退休导出、完整 RET 选择和完整 CFG 别名在物化时共享该对象。部分选择创建新的 Result 对象；即使 block 参数包含下游请求的全部 tile，只要少于原 producer 的集合，也适用此规则。外部直接绑定只有在选定 tile 覆盖整个 captured footprint 时才复用捕获的 Result。身份状态只保留 Result 弱引用；仅在后继需要原始完整集合时，边界才保留规范对象的强引用，因此部分别名中不再使用的 tile 能够释放。

DenseHost import 明确具有两种身份。Kernel input view 保留外部来源 ObjectId，供关联检查使用。复制后的存储在导出为 Result 时使用新预约的 ObjectId，其完整别名共享该 COPY 对象。将导出的副本传入后续 Run 时，使用副本的 ObjectId。通过显式算子产生的复制值也取得新身份。字节相同本身不能建立 ObjectId 相等关系。

具名 RET 仅在执行成功、交付结算结束并通过最终 stop/currentness 检查后发布。较晚 call 失败不会使已经退休的结果失效。绑定源图的 Plan 保持 graph-currentness 语义，独立导入 Plan 不依赖源图。Host/Demand facade 管理 binding replacement：使旧 Run generation 失效并请求 abort，再启动新 Run。活动 task 保持不可变 binding snapshot 与 owner 直到 drain。

## Compiler 产物

产物 header 为 `psir 5` 和 `psxp 5`。IR 因 call 顺序改为退休顺序并显式声明 effect 而升级。Plan 保存全部物理表、unchecked 语义模式、源 identity、kernel 契约版本、输出选择和资源上界。规范字段编码独立于 C++ padding，不保存进程指针、native handle、Run 状态或缓存内容。

Import 执行有界解析、结构校验、symbol/resource 链接并发布不可变对象，不运行 frontend、optimizer、mapper 或 planner。Import 只恢复静态 Plan 数据并解析静态契约/资源 identity。设备对象只在随后显式 prepare 阶段构造。Digest 标识内容，不认证 native code。IR、Plan 和 operation manifest 需要新的 identity domain。v4 产物以重新编译诊断拒绝，不提供兼容转换。

| 旧边界 | 已实现替换 | 迁移 |
| --- | --- | --- |
| IR 顺序 | 有序退休、依赖驱动发射 | IR 语义、optimizer 合法性和 identity |
| PlanStep/PlanTileInstruction | Task/Tile/Call/Block 表 | Builder、校验、选择和 codec |
| Continuation ABI 7 | 固定 invocation、异步 backend completion | ABI 8 和 traits 30，重建插件，无 continuation shim |
| Need/Publication/certification | 静态读取与预分配写入 | 删除 polling、grant assertion 和自动数值认证 |
| Shared stage/checkpoint/block | SSA 中间 task 和 scratch | 删除 executor checkpoint/block/result 复用 |
| Residency/fallback | 每 tile 一个 domain，显式 COPY | 删除隐式镜像缓存与自动重试 |
| Prefix observer | 有序 CallRecord 与最终 RET | 替换逐 tile 前缀通知 |
| Package/data schema | 独立版本轴 | Package 0.39.0 / SOVERSION 0.39；schema encoding 6 保留结构性 publication metadata |

当前实现已移除历史 ADR 中的 continuation、payload cache、隐式 residency、storage reuse 和 Need/certification 执行路径。ADR 0016 现描述不可变 Run input；ADR 0018、0019、0021、0022、0023 已标注被取代的执行部分。兼容的输入所有权、固定网格几何和产品边界保持。当前架构页面描述物理实现；既有 ADR 的 Accepted 状态不改变本提议的正式决策状态。

## 原型与实现验收

C++ 物理数据层定义于 [`physical_program.hpp`](../../../include/photospider/compiler/physical_program.hpp)。`physical::finalize_program` 推导邻接表、跨块存活值和分配上界；`physical::validate_program` 独立检查完整表，包括块参数的物理兼容性。内部 `TaskSchedule` 拥有 block 内的就绪状态、submission identity、failure cut 和 retirement。`CpuWorkers` 在有界 worker pool 上执行固定的调用/完成回调，并在关闭时排空已接受工作。`BlockMemory` 在分配 payload 前预留Host、Shared 与 Device 各域的完整输出上界、每个 lane 的 scratch 和存储 metadata。物理存储在最后一次使用后释放，block 保留预约容量直至边界；跨越边界的 export 此后只保留自身 allocation 的计费。`test_physical_cpu_schedule` 将这些组件组合起来，使用独立 payload allocation 验证真实 CPU 重叠执行。公共 compiler 通过 `OperationProgram` lowering 生成物理表。`CompilerArtifact` 编码 v5 表，并在导入时校验邻接表、存活值、资源预留和 IR 绑定，不重新执行 lowering。固定调用 ABI 8 提供直接 CPU 入口和异步 GPU 提交契约。可信 C++ CPU 算子可显式采用独立 typed invocation/preparation ABI 9 及 Run work/stage 服务，v8 记录和 module 8 transport 保持不变。`KernelPreparation` 携带实际 task 的读写 layout、port、参数字节和 scratch 上界；每个 kernel 在返回 unchecked 入口前检查调用契约。只包含 COPY 的定义无需 kernel preparation callback。公共 `ExecutionContext::prepare` 链接固定 CPU kernel、Host 域异步 GPU 入口和使用 Shared 与 Device 存储的原生 Metal/Vulkan kernel；`submit` 捕获绑定并启动 coordinator；`RunHandle::join` 等待已接受任务及 observer 交付完成。`test_fixed_execution` 验证并发发射、有序退休、observer 失败、取消排空和独立于 context 存活的 Result。`test_fixed_cfg` 验证循环、同时传递边参数和不可变输入绑定。直接绑定要求独立且兼容的 tile allocation。DenseHost 输入 lowering 生成显式 CPU COPY，包含零 call 边界。内置算子使用固定 CPU lowering/preparation/invocation；identity 使用显式 COPY。注册不保证每种 profile 或 layout 模式均可用：依赖 ISA 的 profile 可以不可用，需要独立物化时会拒绝 format 的非空 `layout=view` 请求。Metal adapter 准备固定 MSL pipeline，通过 command-buffer completion 结算已接受工作。Vulkan 准备固定 SPIR-V pipeline 和不可变 uniform buffer；设备完成线程排空 submission fence 后通知 coordinator。

运行 `python3 docs/kernel-architecture/prototypes/ooo_executor.py` 执行语义模型，添加 `--test` 运行行为检查。逻辑时间事件和独立 byte buffer 验证显式/读取依赖、failure cut 与 task 错误归属、独立退休/RET hold 和 retirement。它只模拟一个 block，并独立提供同时传递边参数的 helper；不执行完整 CFG，不实现交付 callback 或资源向量/metadata 容量，也不提供 native backend、公共产物 parser 或性能测量。

Native allocation preparation 针对具体设备解析每个 Shared 与 Device 输出和 scratch 的容量，保持 Plan 不变。Block admission 一次性预留 Host、Device、Shared 和 Payload 容量，包括设备要求的向上取整。各 allocator 均从该预约取得 payload 切片；native 容量变化或不满足对齐时，在发射前拒绝 block。导出的存储在 block 与 device wrapper 释放后仍保留自身切片。`test_native_block_memory` 验证 native allocation、容量拒绝、混合域 Result coverage，以及通过 Shared 与 Device binding 执行 shader。

`PlanningOptions::input_domains` 固定外部输入所在的内存域，默认 Host；`DenseHost` 只接受 Host。CFG 参数继承传入 allocation 的内存域，不兼容的汇合需要显式 COPY。Result 在同一 descriptor coverage 与 ObjectId 下分别持有 CPU 和 Device fragment。CPU window 与 Device fragment 相交时返回 `BackendUnavailable`；内部 Device acquisition 保持 captured descriptor 的授权，并只持有所选 allocation。`test_device_result` 通过公开 prepare/submit/join 执行 native Device producer，验证有序 call export、同设备输入复用与 foreign-device 拒绝。`test_native_copy` 覆盖九种域组合、仿射字节布局、1/2/4/8-byte 样本、staging 拒绝、artifact 导入、并发 Run 和最终资源释放。

Coordinator 在调用 `submit_gpu` 前设置完成槽。`maximum_gpu_submissions` 限制整个 context 内已接受的工作数；每个 Run 在 block 发射前预留完成记录及其 GPU lane 在各内存域的 scratch 最大需求。拒绝提交由 coordinator 本地结算。已接受工作持续持有 invocation view、scratch 和 kernel owner，直至收到匹配的终态完成，包括取消、关闭和非法 acceptance 的情况。错误 completion key 会中止 Run，但不能结算真实 submission。完成可以在提交返回前发生。`test_fixed_async` 使用受控 Host 域 fixture 验证这些所有权和额度规则，不提供原生设备执行证据。

Native kernel service ABI 2 在 preparation 时校验 shader binding 并复制固定 dispatch metadata。Prepared kernel 在 cache 清除后仍持有 pipeline。Native GPU Plan 可以组合 CPU-only 数值 call、GPU 数值 call 和显式 COPY task；同 call 也可在声明能力范围内混合固定 CPU/GPU task。Host/Shared COPY 可采用 CPU placement；Device endpoint 要求 GPU COPY。`test_fixed_native` 通过 C11 module 执行真实 Metal shader，覆盖 tile 依赖、独立 call、artifact 导入、部分输出选择，以及绑定不同外部输入的并发 Run。设备 command 失败会中止 Run，同时排空已接受 submission。`test_vulkan_gpu` 验证 Vulkan 固定路径、显式 COPY、独立中间输出、并发提交和完成线程中的 Device 析构。这些测试未覆盖真实设备故障注入。

Focused validation 覆盖 compiler-to-Plan-to-CPU 真实重叠、非法 artifact、selection/loop、异步 native completion、显式传输、取消/失败后的 owner drain、有序 observer 交付和导出值释放后的计费。终态优先级回归在持有 accepted work 时验证 device-first 与 observer-first 两种 abort 顺序；静态 CPU/GPU 混合 lowering 同时验证跨 call 与同 call 场景。[原生测量](Parallel-Execution-Model.zh.md#原生测量)记录 Metal/Vulkan 每种 workload 五次 Run、完整 block 容量拒绝、COPY、drain 和独立关联的设备时间。当前基线不声称覆盖物理 device-loss 注入或可选多 task packet。内核开发期间仍暂停安装消费测试。

## 限制

Shape 与单 block task 图固定。分支预测、跨迭代投机、运行时依赖发现、固定容量 task ROB、payload 多级缓存、自动重算、存活 tile 别名和隐式迁移不在范围内。永不终止的 callback 或无法确认停止访问的设备可能无限延迟 drain；超时不能授权提前释放 buffer。
