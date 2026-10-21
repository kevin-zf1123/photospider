# 计算边界

[英文权威文档](../Compute-Boundaries.md)。

## 1. 模块职责与所有权

| Owner | 职责 | 生命周期边界 |
| --- | --- | --- |
| `GraphContext` | 复制的 workflow source、revision 与 snapshot currentness | 源变更使 source-bound version 失效，不改变活动 Run 的 binding |
| `Compiler` | 静态特化、ValidatedIR、可选优化、物理 lowering 与校验 | 产生不可变表，不执行 sample payload |
| `ExecutionPlan` | CFG、Task/Tile/Call 表、layout、placement、read、selection 与资源上界 | 共享不可变 program，与运行时 input address 分离 |
| `PreparedPlan` | 固定 kernel 链接、具体 device 兼容性、静态 resource 与 module owner | 在原 context 的多个 Run 间共享，拓扑保持固定 |
| Run 与 `RunHandle` | 不可变 binding、activation/task/call 状态、就绪、退休、取消与诊断 | Accepted work 在 drain 期间保留 owner；join 等待 task 与 delivery settlement |
| `ExecutionContext` | 有界 CPU worker、GPU credit、公平准入、冻结 registry 与资源根 | 析构时关闭准入并排空已接受 Run 与 callback |
| Operation definition | 静态契约、lowering、kernel preparation 与数值实现 | 在 unchecked invocation 前声明全部 read、write、scratch 与 completion 义务 |

## 2. 数据与内存边界

`ExecutionBindings` 提供 owning Result 引用。Submission 为单次 Run 捕获 schema、coverage 与 storage authority。调用方随后发布内容或替换 binding，不会扩大该 snapshot。每个新产生的动态 value 取得新 identity；同一完整 value 的 alias 保留 identity。返回 Result 和 owning read window 通过持有 release context，可以超过 Run 与 context 生命周期。

物理 tile 拥有独立声明的 allocation。Region 描述逻辑坐标，affine layout 将其映射到固定 allocation view。Native preparation 解析实际 capacity 和 alignment。Block admission 在第一个 task 前预留完整 output backing、逐 lane scratch、completion 与 delivery capacity。预算拒绝返回 `ResourceExhausted`，不发射该 block。

Payload backing 与 managed metadata 分别观测。Context/PreparedPlan 和既有 input owner 保持原归属；Run 新增 metadata 在跨线程释放时仍归属于原资源根。Reservation 不等于实际分配，两者都不描述 RSS。参见[托管资源](Managed-Resources.zh.md)。

## 3. 执行与完成

Planner 在执行前展开全部依赖。Coordinator 发射满足 dependency、lane、context-credit、effect 与 failure-cut 约束的 ready task。CPU worker 执行固定同步入口；accepted GPU submission 异步完成。Task 成功后，其输出立即可供后继 task 使用。Call 按动态 IR 顺序退休，与 task 发射/完成顺序独立。

`CpuExact` 要求 CPU placement。`NativeGpu` 是静态编译选择：支持 GPU 的 operation 收到 GPU lowering 请求，CPU-only operation 收到 CPU 请求。Traits 允许时，operation 可在同 call 内显式混合 CPU/GPU task。COPY task 实现声明的传输；Device endpoint 要求 GPU COPY。Preparation 对不可用 placement 直接拒绝，不改写 task；执行期间不切换 backend 重试。

每次 block activation 消耗有界的 `maximum_block_activations` 与 `maximum_work` 预算，空 loop 同样消耗 work。Coordinator 在安全点检查取消与源 currentness。执行期间不扩展 task graph、schema 或 dependency。数值工作遵守 operation 的 prepared contract，不使用 continuation work-service 协议。

拒绝提交时，invocation owner 留在 coordinator，且不产生 completion。接受后保留 input/output/scratch、参数、kernel/module owner 和预留的 completion record，直到恰好一次 terminal event 确认停止访问与可见性。取消停止新工作，并排空已接受工作。超时不能授权提前释放 buffer。

有序 call delivery 在 coordinator 外运行，每个 Run 有独立串行 lane。Observer 失败会 abort Run 并停止后续交付。最终 RET publication 等待 task quiescence、delivery settlement 以及最终取消/currentness 检查；已经交付的 owning value 保持有效。失败优先级、公平性、计时和原生测量见[并行执行](Parallel-Execution-Model.zh.md)。

## 4. 公共边界

公共 API 暴露不可变 Plan/preparation、binding、`RunHandle`、取消、join、owning Result 与独立 observation。Queue entry、native device handle 和可变 scheduler state 保持内部实现。Handle 是进程内对象；daemon 独立负责 IPC 与 Job 生命周期。

Kernel 不提供持久 Run、document persistence、result recovery、运行时依赖发现、payload cache hierarchy 或隐式迁移。Provider code 受信任，必须在报告 completion 前结束全部访问。
