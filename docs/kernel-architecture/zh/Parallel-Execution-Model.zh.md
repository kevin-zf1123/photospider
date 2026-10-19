# 并行执行与有序退休

状态：Proposed breaking design，实现状态见 [Plan 契约](Compiler-and-Execution.zh.md)。[英文权威文档](../Parallel-Execution-Model.md)。

## 范围与职责

每个 Run 的 coordinator 是状态、依赖计数、ready queue、call record 与 hold 的唯一修改者。Context 提供 Run 共享的有界 CPU pool 和 GPU submission queue。Worker 与设备 callback 写入预留完成记录，不修改依赖或退休状态。

```text
                        Run coordinator
                  /            |            \
             CPU ready      GPU ready    calls + retire_head
                 |              |                |
             CPU workers    GPU submit      ordered delivery
                 \              /
                reserved completion records
                           |
                    Run coordinator
```

多发射表示在一次 coordinator 推进中准入所有符合条件且资源允许的 task。各 backend 的 ready heap 按 `(call ordinal, task ID)` 排序，context 公平轮转可执行 Run。依赖处理为 O(T + E)，heap 增加 O(T log T)，不模拟硬件时钟。

## 状态与不变量

```cpp
#include <cstdint>
namespace ps_executor_design {
enum class TaskState : std::uint8_t {
  Waiting, Ready, Submitted, Succeeded, Failed, Skipped
};
struct TaskRuntime { TaskState state; std::uint32_t pending; };
struct CallRuntime {
  std::uint32_t unsettled, first_failed_task;
};
struct CompletionKey {
  std::uint64_t run, activation;
  std::uint64_t submission;
};
}  // namespace ps_executor_design
```

`pending(t)` 统计尚未成功完成的不同 predecessor。同一 producer 的两个输出只等待一个事件。生命周期 hold 则按不同 `(consumer task, tile)` 计数；两个端口读取同一 tile 只持有一次。Producer 完成前，未来消费者的 hold 已全部建立。

每次被接受的提交恰好拥有一条终态完成记录。SubmissionId 映射到一个 task 或不可变 packet 成员范围，与 TaskId 独立。Coordinator 先检查 submission key/state 一次，再将事件分发到各成员状态。`Submitted` 包括已排入 worker 的 CPU 工作和已接受的设备工作。成功表示全部输出写入以及 input/scratch 访问结束，并具备所需可见性。拒绝提交不转移所有权，也不产生 callback，由 coordinator 处理局部失败。成功一次性公开 task 的全部输出；失败不公开输出。

执行额度和 scratch 在完成时归还。Call 的全部成员进入终态后视为 settled，只能在 `retire_head` 退休。选中的 call 只有在全部 task 成功时才成功；零 task 纯 call 以 `Elided` 退休。100 个 task 的 call 可以使用两个执行槽，因为执行槽不等待退休。

成功处理依次标记输出可用、访问每个 successor 一次、结算 call 成员、释放 input hold/scratch 并归还额度，然后退休连续的 settled 前缀并准入新工作。消费者就绪不等待退休。用户交付独立执行。

```text
Two CPU slots:
call order:       A(10), C(A,1), B(2), D(B,1), E(C,D,1)
issue order:      A, B, D, C, E
completion order: B, D, A, C, E
retirement order: A, C, B, D, E
elapsed ticks:    12
```

## 准入与推进

第一版在 block 第一个 task 之前预留全部输出、持久 frame 状态、有界交付存储、可用 scratch lane 和完成容量，计算公式见 [tile 存储](Cache-Model.zh.md)。失败时返回 `ResourceExhausted`，本 block 没有 task 开始；先前 block 的结果和 effect 仍可观察。

Task 必须处于活动、未提交、依赖就绪、failure cut 之前的状态，并取得全部额度。Effect 还要求其 call 等于退休头。Scratch lane 忙时可暂时绕过，通过公平选择重新访问。Task 原子获取完整的跨内存域资源向量。

Coordinator 接收事件、应用 stop、跳过工作、退休 call 并填充 lane。只有已接受工作或具体 context-credit 事件能够推动进展时才等待。Block reservation 被拒绝后失败，不无限等待。成功准入后，若仍有未完成工作但没有在途或可执行 task，属于 `Internal` 不变量错误。

推进保证以有限 DAG、同 call 或更早 call 的依赖、有限 callback、设备最终完成、context 公平服务及完整预留为前提。最早未完成前缀 call 必有在途工作，或有 predecessor 能够完成的 task。当前工作完成后，该 call 的 ready task 可装入保底 lane，每次完成减少未结束工作。这是上述假设下的资源与调度保证，不是任意 native code 的终止证明。

## 失败、副作用与取消

`failure_cut` 是已经观察到 task 失败的最小 call ordinal，初始为 call 数量。严格早于 cut 的工作继续运行。Cut 及之后尚未提交的工作被跳过，已经接受的工作必须 drain。失败不满足成功依赖；被跳过消费者的未来 hold 释放一次。更早 call 的失败事件可以继续向前移动 cut。

先退休成功的较早前缀，再在失败 call 的已接受成员全部停止访问后退休一条失败记录。更晚 call 不退休，最终错误必须等待全部更晚在途工作也排空。同一 call 多个 task 失败时，从实际失败 task 中选择最小静态 TaskId；不为被跳过 task 编造错误。该规则对已观察失败稳定，不承诺执行所有 sibling 以发现每种可能错误。Failure-cut 使用独立内部停止机制，不得通过用户取消 token 取消较早前缀。

原型为每次实际失败的 completion 在 `task_failures` 中记录 `TaskFailure(activation, task_id, call, task_name)`。失败退休时，`retired_failures[call]` 记录该 call 中最小已观察 TaskId。终态 `failure` 只在 Run 为 `OperationFailed` 时设置；取消保留已观察/已退休失败诊断，但不把它们作为最终原因。跳过的 task 不生成失败 detail。Packet 级错误归属独立处理，当前原型尚未覆盖。

Effectful call 包含纯 preparation 与一个终端 effect task。它仅在退休头且自身 predecessor 成功后启动，因此更早 call 已全部成功退休。更晚纯计算可能已运行。Effect 失败前已发生的外部写入不能回滚。多个有序外部 effect 需要拆成不同 call，或由算子实现事务协议。

用户取消属于 Run abort。Coordinator 关闭准入、跳过未提交工作、排空已接受工作并停止进一步退休。已移交的记录继续有效，交付可以继续完成。终态锁定前，已观察用户取消优先于 Stale，再优先于 context/device/observer abort，最后是 task 局部失败；context/device/observer 类别内部保留 coordinator 首先观察到的原因；锁定后新请求不再改变结果。设备永久失败导致更早工作无法完成时，使用 context/device abort，不承诺普通的成功前缀完成。

图或 binding 失效以及 observer 失败同样 abort。同 Run callback 串行化、交付失败与终态锁定遵循[返回接口](Compiler-and-Execution.zh.md#run控制流与返回接口)。Device loss 必须确认 DMA/callback 停止访问，才能释放存储，backend 在此之前保留 lease。虚假成功、逃逸 CPU 工作或越界写入违反受信任算子契约。

## CPU 与 GPU 完成边界

CPU invocation 接收不可变输入 view、独占预分配输出、私有有界 scratch、参数和 cancellation。同步 `invoke` 在全部访问与内部工作结束后返回 status。Worker 以 release 顺序发布，coordinator 以 acquire 顺序消费。基础 kernel 每 task 使用一个 worker；并行阶段降低为 task，不通过嵌套 pool job 阻塞 worker。

Backend 逻辑接口为 `SubmitResult submit(const Invocation&, CompletionSlot&) noexcept`，返回 `Rejected(Status)` 或 `Accepted(Ticket)`。接受后转移 owner，直到恰好一次 completion；拒绝时 owner 留在 coordinator，且不排入 completion。Submit 前先准备 slot，因此允许同步完成。Callback 只写有界 status 并唤醒，不分配、不运行用户代码。

GPU encode 和 queue 接受不使输出可读。Input/output/scratch、参数、command、pipeline/module owner 和 completion slot 保持到设备停止访问。Host 可见完成包含必要 cache maintenance。GPU 消费者需要 access/layout transition 与内存可见性，共享内存也需要同步。跨域传输使用显式 COPY task。

Completion key 与 queue timepoint 分离。Task 7 完成不代表 task 0 至 6 完成。GPU timeline 按同一有序 queue 的实际提交顺序编号，独立 queue 使用独立 timeline。TaskId 和 CallId 不得作为通用完成计数器。

### Device packet

基础设计逐 task 提交。可选 compiler packet 将同设备子图合包，要求全部外部输入边在提交前满足，全部对外输出边在 packet 完成后可用。第一版 packet 不含 effect，并只属于一个 call。收缩后的 packet 图必须无环；围绕一个依赖 B 的中间 task 合并两侧 A task 可能产生 A/B 环。

Packet 消耗一个 backend submission credit 和一个 completion slot，其全部成员在该单一事件处理前保持 Submitted。设备 barrier 实现内部依赖。提交前预留全部输出和完整 packet scratch/command 上界。成功时在一个事件中按拓扑顺序结算成员。缺乏逐成员失败信息时以 `PacketFailure(submission, members)` 使整个 call 失败，不公开 packet 输出。不编造首个失败成员，packet 级来源与 task 局部错误选择分开记录。限制为一个 call 可保持跨 call 失败归属；跨 call 链在 backend 提供充分逐 task 完成语义前保持独立提交。合包可能延迟消费者，需要与提交开销一起测量。

## 诊断与检查

分别测量 ready、submit、start、device completion 和 retirement，保留 backend 在途峰值、ready/scratch 等待、传输及 drain 延迟。只对可比较时钟域的时间戳作差。第一版 C++ 实现额外输出以下指标：

| 指标 | 定义 |
| --- | --- |
| `reserved_bytes_by_domain` | 按内存域统计当前准入预算容量，包含未使用的 block reservation；每个 lease 计一次 |
| `live_bytes_by_domain` | 各域当前实际分配的受控字节，包含保留的返回 owner；按 allocation 大小计费，不等同于逻辑图像大小或 RSS |
| `allocation_count` | Run 内成功物化 tile allocation 的累计数量，排除 scratch、command 和借用的 input owner |
| `block_admission_failure` | 准入失败次数及 block/activation、资源维度、所需容量和可用容量；在该 block 发射前报告 |
| `retirement_head_wait` | Call 成为退休头至其 settled 且可退休的单调时钟时间；不含 callback 交付延迟，abort 时单独报告尚未结束的等待 |

同时报告 reserved/live 当前值与峰值。Block 结束后归还未使用 reservation，导出 owner 保持计费；Run 完成后释放这些 owner 仍更新计费。Block 内 live bytes 下降不表示 reserved bytes 同步下降。[容量示例](Cache-Model.zh.md#容量成本与测量)量化这一基线成本。

第一批 native GPU 评估覆盖四种独立工作负载：

| 工作负载 | 必需观察 |
| --- | --- |
| 同 call 大量 tile | 吞吐、packet 大小、提交数、分配数和容量峰值 |
| 独立 calls | 真实重叠、queue 占用和退休头等待时间 |
| 相互依赖的 GPU call，例如 blur -> color transform -> blend | Device complete 到下一次 submit 的间隔、各 call 延迟和 host 唤醒次数 |
| 带显式 COPY 的 CPU/GPU 链 | 传输字节/时间、host/device 同步与 drain 期间的生命周期 |

在基于完成事件判断就绪的规则下，同 call packet 不能预先提交存在依赖的跨 call 链。先测量这段 host 推进间隔，再决定是否引入更细设备完成点或跨 call 编排。比较时固定 shape、tile extent、placement、worker/queue grant 和资源限制，并记录物理设备与 driver。Block 合并必须与容量和准入失败一起评估；在独立论证替代策略之前保留全 block reservation。这些是实现验收测量要求，不是 Python 模型的测量结果。

原型检查不增加 payload hold 的显式顺序、独立退休/RET 所有权、已观察 task 错误选择、退休前转发、大 group、failure cut、effect、取消和释放。它使用逻辑 tick，并为可读性扫描/排序任务表。C++ 使用预编译邻接表、依赖计数、ready queue 和 call 未结束计数。剩余检查包括完整 CFG/loop、选择闭包重建、callback 交付和 finalization 竞争、多域/metadata 容量、Accepted/Rejected 所有权竞争、inline completion、device loss、可见性和真实 packet 完成/失败。

## 一手设计参考

- [BOOM ROB](https://docs.boom-core.org/en/latest/sections/reorder-buffer.html)：有序 commit 与队头异常处理为 call record 和退休游标提供参考，不引入投机寄存器状态。
- [IREE Stream](https://iree.dev/reference/mlir-dialects/Stream/) 与 [HAL](https://iree.dev/reference/mlir-dialects/HAL/)：异步资源与 wait/signal fence 为显式完成依赖提供参考。
- [CUDA Graphs](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cuda-graphs.html)：分离定义、实例化与执行，为 prepare-once 数据和可选 packet 提供参考。
- [Metal completion](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addcompletedhandler(_:))：设备完成通知支持区分 submit 与 Done。
  Apple 的[归档 command guide](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html)（2016-12-12 更新）同时说明 command-buffer 保活与快速完成 handler；这里只引用这些基础生命周期机制。
- [Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)：执行顺序、可用性和可见性需要显式 backend 同步。

这些来源为特定机制提供依据，不构成 Photospider 已实现该执行器的证据。
