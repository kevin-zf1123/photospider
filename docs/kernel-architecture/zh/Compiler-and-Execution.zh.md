# Compiler、Plan 与 Executor

状态：Proposed breaking design。本页与[并行执行](Parallel-Execution-Model.zh.md)、[tile 存储](Cache-Model.zh.md)、[依赖](Dependency-Data.zh.md)替换执行设计。下列接口是规格草图，不是已安装 API。生产 C++ 仍实现顺序 continuation 执行器。[参考模型](../prototypes/ooo_executor.py)独立验证新的调度语义。[英文权威文档](../Compiler-and-Execution.md)。

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

`FrontendRegistry`、`OptimizerRegistry`、`PlannerRegistry` 保持具名注册和冻结后的共享所有权。规划独立于可选优化。Planner 通过私有 builder 构造 task/tile/call/block 表，由 compiler 校验完整结果后发布，替换只能修改指令 vector 的 builder。未知名称返回 `NotFound`，非法生成程序返回 `Internal`，格式错误的导入程序返回 `InvalidArgument`，不兼容契约返回 `TypeMismatch`。

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

`prepare_kernel(symbol, static_metadata, parameters, resources, device)` 生成不可变 `PreparedKernel`，包含直接调用入口、解码后的 layout、resource owner 和 pipeline。PreparedPlan 保留这些 context 和模块；invocation 借用其 context 直到完成。所有引用 Run 排空后才能销毁 context。

`Placement` 固定 backend 类别、数值 profile、内存域和 queue 能力。`prepare` 将其绑定到具体设备对象和 kernel，可以拒绝不可用 backend，但不得改变 task 拓扑、实现 profile 或 COPY 边。CPU/GPU 替代方案需要分别编译的 Plan。提交后没有隐式重试或 placement fallback。

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
4. 每个 task 恰好属于一个 call 和 block，call ordinal 连续。Producer call ordinal 不大于 consumer call ordinal。Task 数组位置不约束执行顺序。
5. 每个 effectful call 在全部纯 preparation task 后有一个终端 effect task。它不再指向本 call 的 preparation，并且只在退休头获得执行资格。
6. 执行路径产生必需条件、return、边参数和支配的跨 block 存活值。只为选中的后继建立动态 activation。
7. 资源向量覆盖输出、持久状态、descriptor、command metadata、队列/完成记录、交付记录和 scratch。Backend 支持声明的同步。

这些检查校验声明，不证明任意 native 实现。算子负责在报告成功前写完全部声明输出，并遵守数值语义。未声明访问或非法数值前置条件违反 unchecked 契约，不保证产生诊断。

`select_outputs` 在已有表上求子集闭包，保持 control/effect 和 call 顺序，重建 task 归属、邻接表、return hold 和资源上界。它不调用 mapper，也不增加缺失 tile。超出已编译覆盖的请求需要重新编译。空选择仍执行 control 和 effect。

## Run、控制流与返回接口

`ExecutionPlan` 持有可序列化的不可变程序。`PreparedPlan` 持有已链接 kernel、设备对象和模块/资源 lease。`RunHandle` 持有可变状态与不可变 binding lease，并保留 prepared program 直至 drain。并发 Run 共享 prepared code 和 context pool，输出分配相互独立。

逻辑 API 为 `prepare(plan, device_context)`、`submit(prepared, bindings, options)`、`request_cancel(run)` 和 `join(run)`。可能失败的操作返回类型化 status。`join` 等待全部已接受提交停止访问资源，返回完整具名 Result 集合或一个终态错误。Result 通过保留 allocation release context，可以比 Run 和 context 活得更久。

可选 `on_call_retired(CallRecord)` 接收 sequence、activation、来源 call、status 和选定导出值。健康 observer 为每个退休 call 接收一条记录，包括 elided call。不导出的中间值没有交付 hold。Block admission 为有界 delivery queue 预留存储；用户 callback 在 coordinator 外执行，与完成事件处理相互独立。

每个 Run 有一条不可重入的串行 delivery lane。Callback n 正常返回后才启动 n+1，因此调用开始与正常返回的顺序都遵循退休顺序。不同 Run 可以并发交付。`join` 等待 task drain 和交付结算；callback 不得同步 join 自身 Run。慢 callback 可以延迟 join，或占用下一 block 所需容量，但不能阻止 CPU/GPU 完成事件处理。

Callback 抛异常时立即关闭该 Run 的 delivery lane：不再启动后续 callback，丢弃已排队记录并释放其 delivery hold，用户独立保留的值继续有效。Lane 关闭与 enqueue 串行化。关闭后的 enqueue 拒绝记录且不接管 owner；coordinator 释放对应 delivery hold 并将记录标为 settled，包括与失败通知竞争的退休。Delivery lane 发布一条预留 settlement event，原子携带 `OperationFailed`、observer origin 和对应 CallSeq。失败 callback 的记录在 coordinator 消费该事件前保持 unsettled；coordinator 在同一次状态转换中先记录 abort，再将该记录标为 settled。禁止先报告成功 acknowledgment，再单独发送错误事件。Coordinator 停止后续准入与退休，并排空已接受工作。Callback 失败前，退休可能已经允许更晚的 effect 执行；这些 effect 无法撤回。

最终 RET 数据保持内部持有，直到全部已接受工作停止访问，且 coordinator 确认所有退休记录已经完成交付结算，包括最后一个 callback。未设置 observer 时，记录立即结算。Coordinator 在一个串行化 finalization 点处理待决 delivery/stop 事件并锁定终态；仅成功时才把最终 RET owner 转交调用方。因此，即使全部 kernel 已完成，最后一个 callback 的异常仍会阻止成功 RET。在该点之前观察到的取消保持已定义优先级；该点之后的请求不能改变终态。原型直接模拟拥有型退休记录，尚未实现此 callback 协议。

进入 block 时为新单调 activation 建立全部 task/call 状态。成功 terminator 在 block 的 call 全部退休且 task 全部完成后运行。Coordinator 读取 host 可见条件，同时捕获所选边的全部参数和跨 block 存活值，取得下一 frame 的 hold，再释放旧 frame。Compiler liveness 包含跨越直接后继使用的支配值，即使它们不是显式 block 参数。未选后继不实例化。

循环每轮创建新的物化 tile identity。传入值保留旧 allocation，直到最后一次读取结束。每次 block entry 都消耗有界 work，包括空循环；activation/sequence 溢出时拒绝继续。Device 中的分支标量需要显式 host COPY。Coordinator 在安全点检查取消与当前性。

具名 RET 仅在执行成功、交付结算结束并通过最终 stop/currentness 检查后发布。较晚 call 失败不会使已经退休的结果失效。绑定源图的 Plan 保持 graph-currentness 语义，独立导入 Plan 不依赖源图。Host/Demand facade 管理 binding replacement：使旧 Run generation 失效并请求 abort，再启动新 Run。活动 task 保持不可变 binding snapshot 与 owner 直到 drain。

## Compiler 产物

目标 header 为 `psir 5` 和 `psxp 5`。IR 因 call 顺序改为退休顺序并显式声明 effect 而升级。Plan 保存全部物理表、unchecked 语义模式、源 identity、kernel 契约版本、输出选择和资源上界。规范字段编码独立于 C++ padding，不保存进程指针、native handle、Run 状态或缓存内容。

Import 执行有界解析、结构校验、symbol/resource 链接并发布不可变对象，不运行 frontend、optimizer、mapper 或 planner。Import 只恢复静态 Plan 数据并解析静态契约/资源 identity。设备对象只在随后显式 prepare 阶段构造。Digest 标识内容，不认证 native code。IR、Plan 和 operation manifest 需要新的 identity domain。v4 产物以重新编译诊断拒绝，不提供兼容转换。

| 边界 | 替换契约 | 所需实现修改 |
| --- | --- | --- |
| IR 顺序 | 有序退休、依赖驱动发射 | IR 语义、optimizer 合法性和 identity |
| PlanStep/PlanTileInstruction | Task/Tile/Call/Block 表 | Builder、校验、选择和 codec |
| Continuation ABI 7 | 固定 invocation、异步 backend completion | 目标 ABI 8 和 traits 30，重建插件，无 continuation shim |
| Need/Publication/certification | 静态读取与预分配写入 | 删除 polling、grant assertion 和自动数值认证 |
| Shared stage/checkpoint/block | SSA 中间 task 和 scratch | 删除 executor checkpoint/block/result 复用 |
| Residency/fallback | 每 tile 一个 domain，显式 COPY | 删除隐式镜像缓存与自动重试 |
| Prefix observer | 有序 CallRecord 与最终 RET | 替换逐 tile 前缀通知 |
| Package/data schema | 独立版本轴 | 实现时分配 package/SOVERSION，移除 publication-scope 字段时修订 schema 编码 |

本提议替换 ADR 0016 的 continuation/前缀通知、ADR 0018 的运行时缓存、ADR 0019 的隐式 residency、ADR 0021 的存储复用例外、ADR 0022 的 staged ABI，以及 ADR 0023 的 Need/认证/证据复用等执行相关部分。兼容的输入所有权、固定网格几何和产品边界保持。既有 ADR 的 Accepted 状态不表示本提议已接受或实现。其他架构页面在迁移前继续描述当前 C++ 实现。

## 原型与实现验收

运行 `python3 docs/kernel-architecture/prototypes/ooo_executor.py` 执行语义模型，添加 `--test` 运行行为检查。逻辑时间事件和独立 byte buffer 验证显式/读取依赖、failure cut 与 task 错误归属、独立退休/RET hold 和 retirement。它只模拟一个 block，并独立提供同时传递边参数的 helper；不执行完整 CFG，不实现交付 callback 或资源向量/metadata 容量，也不提供 native backend、公共产物 parser 或性能测量。

生产验收需要 compiler-to-Plan-to-CPU 的真实重叠执行、非法 artifact 拒绝、selection/loop 覆盖、异步 native GPU 完成、显式传输、取消/失败后的 owner drain、有序 observer 交付，以及导出值释放后的计费检查。Native 声明必须有物理 GPU 执行证据。内核开发期间仍暂停安装消费测试。

## 限制

Shape 与单 block task 图固定。分支预测、跨迭代投机、运行时依赖发现、固定容量 task ROB、payload 多级缓存、自动重算、存活 tile 别名和隐式迁移不在范围内。永不终止的 callback 或无法确认停止访问的设备可能无限延迟 drain；超时不能授权提前释放 buffer。
