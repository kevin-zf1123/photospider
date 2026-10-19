# Tile 存储、生命周期与准入

状态：Proposed breaking design。本页以唯一 tile 存储替换 executor 缓存设计。参见 [Plan 契约](Compiler-and-Execution.zh.md)和[英文权威文档](../Cache-Model.md)。

## 数据布局与所有权

动态物化 tile 的 identity 为 `(RunId, ActivationId, TileId)`，具有一个 producer、一个 allocation 和一个不可变 domain。不同的同时存活物化 tile 使用独立 backing allocation。消费者共享同一 tile 的只读 handle。View 与 block 参数名称引用同一个值，不产生第二次物化。

第一版在 admission 时分别分配当前 block 的输出 buffer，不使用大 slab 切分多个逻辑输出 buffer。旧 allocation 最后一个 owner 释放后，allocator 可以复用地址；Plan 不为存活 tile 建立别名。Scratch 属于提交，在完成后可以复用 lane。

外部 Result binding 可以使用已有合法 Tensor backing。Preflight 检查 name、schema、coverage、layout 和 owner，不扫描样本。显式 import COPY task 将所需外部 tile 物化为 Plan 表示，归属最早消费它的 call，并进入依赖图。零拷贝绑定只允许独立、兼容、不可变的 tile allocation，不创建隐藏 input-copy cache。

Run 在 import/direct read 期间保留 binding。参数、prepared resource、pipeline 和 module 保持到已接受提交结束。返回 Result 保留 tile allocation，以及在 execution context 销毁后释放它所需的 allocator/device release context。

## Hold 与释放

对 tile `v`，维护：

$$
H(v)=P(v)+U(v)+L(v)+R_{\mathrm{call}}(v)+R_{\mathrm{ret}}(v)+X(v).
$$

`P` 是预留/尚未结束的 producer hold；`U` 统计不同的未结束 consumer task；`L` 统计 frame/跨 block/terminator 义务；`R_call` 统计等待 call 退休的 export，`R_ret` 统计等待最终 RET 的值；`X` 统计外部 owning handle。在途 input 访问由 consumer hold 覆盖，output 写入由 producer hold 覆盖。额外 view 必须在这些提交下借用，或取得自己的 owner。

| 事件 | Hold 变化 |
| --- | --- |
| Admission | 在任何 task 开始前建立 producer、全部未来 consumer、跨 block 和 export hold |
| Task 完成 | 结束访问，释放 producer/input hold 与 scratch 一次 |
| 跳过未提交 task | 释放未来 input hold 与未使用 output producer hold |
| Call 退休 | 将选中 export hold 转移给交付记录，或丢弃无用 hold |
| Block 边 | 先取得所选后继/跨 block hold，再释放旧 frame hold |
| RET | 把 return hold 转给最终具名 Result |
| 外部释放 | 释放 owning reference |

仅在 `H(v) == 0` 时释放存储。完成、退休和释放是不同事件。没有 export/跨 block 义务的未退休中间值，可以在消费者结束后释放；已退休输出则在 host 保留时继续存活。Abort cleanup 丢弃不再需要的 live/export hold，但在停止访问之前保留每个已接受提交的 producer/consumer hold。

Call 交付与最终 RET 是独立义务。同一 tile 同时被两者选中时，在 producer/consumer hold 之外再取得两个 hold。Call 成功退休只把 `R_call` 转交 delivery owner。Run 成功时把 `R_ret` 转交最终 result owner；失败或取消时丢弃全部待交付 `R_ret`，即使对应 producer call 已经成功退休。此前已移交的 call value 保持其外部 owner。失败 call 不移交任何值，即使部分 sibling task 已成功。

[原型](../prototypes/ooo_executor.py) 分别以 `retirement_exports` 和 `return_values` 选择两类值。已取得的内部义务位于 `retirement_holds` 与 `return_holds`；退休时转入 `retired_values`，成功 RET 时转入 `results`。这些 dictionary 模拟拥有型结果集合，不表示任意复制裸 handle 就能增加所有权。通过 `release_retired_values()` 与 `release_results()` 独立释放，两种顺序均合法，重复释放空集合无操作。准入失败不取得任何一类 hold。模型把未移交的 export 义务保留到终态 drain，然后丢弃。

分支处 frame 保留所有可能后继 live value 的并集，再丢弃未选 hold。循环的新 activation 在分配新输出时保留传入值。Compiler liveness 覆盖跨越直接后继使用的支配值。Host 保留的 export 跨迭代继续计费。

## Block reservation

Admission 使用 host payload、device payload、metadata、queue 与 completion 容量组成的向量。每个内存域 `d` 的充分容量条件为：

$$
A_d=L_d+\sum_{v\in O_b}\operatorname{allocatedBytes}(v)
       +\sum_q C_q S_{q,d}+M_d.
$$

`L` 为已计入预算的存活传入/export 容量；`O_b` 为新 block 输出和持久临时 tile；`C_q` 为 backend 类别 `q` 获得的在途容量；`S_{q,d}` 为该类允许提交单元（task 或 packet）在域 `d` 的最大 scratch/command 容量；`M` 覆盖 frame 状态、descriptor、queue 与交付记录。大小包含 rounding/alignment，求和与乘积检查溢出。每个 task 原子获取完整资源向量。`A_d` 是总容量要求；新增 reservation 只申请 `A_d - L_d`，不重复计费已有 owner。

Block 使用的每个 backend 类别至少有一个可用 lane。Completion 容量覆盖所有可能在途提交，完成事件 producer 不等待 retirement 或普通工作队列 slot。完成、跳过和退休所需 metadata 全部预留。发射前完成分配；分配失败撤销 admission，本 block 启动 task 数为零。不透明 driver 分配仍可能产生操作性失败。

Reservation 属于 block，其他 Run 不得撤回。Tile 释放的容量在 block 结束前不借给其他 Run，但物理 buffer 可在最后使用时释放。保留 export 以实际容量跨边界计费，其余容量归还 context。Host 保留值可能导致下一 block 准入失败，executor 不无限等待外部释放。

此保守上界以更高内存需求换取明确推进保证。Oldest-ready 优先不足以保证 lazy allocation 安全。例如 `A -> B` 和独立 C，三个输出各 4 bytes，B/C 保留到 RET，预算 8。C 与 A 先完成后都被 hold 保留，B 无法分配。顺序 A,B,C 可在 8 bytes 内完成，整 block 输出预留却要求 12 并拒绝。将来 lazy 策略需要独立证明的保留容量规则。更小的合法调度 block 或更窄输出选择可以降低容量，但减少并行度。

## 容量成本与测量

8192 × 8192 的 RGBA Float32 图像含 `8192 * 8192 * 4 * 4 = 1073741824` payload bytes，即 1 GiB。Extent 为 128 时共有 4096 个 tile，每个 262144 bytes（256 KiB）。若同一调度 block 中每个 call 都完整物化一张这样的图像，仅输出的预留为：

| 完整物化图像值数量 | 输出 payload 预留 | 独立 tile allocation |
| ---: | ---: | ---: |
| 1 | 1 GiB | 4096 |
| 4 | 4 GiB | 16384 |
| 8 | 8 GiB | 32768 |
| 16 | 16 GiB | 65536 |

这些是布局计算，不含 incoming/export 值、scratch、metadata、allocation padding 和 driver 开销，不是 RSS 或耗时测量。临时 tile 可在最后使用后释放物理 buffer，而所属 block 仍持有预留预算。因此按[诊断契约](Parallel-Execution-Model.zh.md#诊断与检查)分别报告 reserved/live bytes、allocation count 和 admission failure detail。

合并 DAG 路径 block 会扩大调度空间，也可能增加准入工作集。对相同 workflow 比较合并 block 与较小合法 block，分别记录容量拒绝、排队延迟和退休延迟。基线保持全 block reservation；按容量划分需要实际收益与明确 compiler 策略，不引入存活 tile 别名或 payload 多级缓存。

## Identity 与缓存边界

IR/Plan identity 用于复现与可选 host artifact 查找。PreparedPlan 复用共享不可变 code/resource。不可变输入 snapshot 工具可以在 executor 外保留，提供 binding owner，不授予执行结果复用资格。

替换后的 executor 没有 completed-result、checkpoint、block-state cache，没有 residency mirror、自动 input-copy cache，也不在热路径进行 tile content hash。每个准入 task 执行一次，除非因 abort/failure 跳过或编译期省略。Host 可按自己的策略把缓存输出绑定为显式输入。

`COPY host -> device` 产生不同 tile。共享 coherent memory 是一个声明的 domain，使用一个 allocation 并执行同步。更改 backend 需要不同 prepared/compiled placement 契约，不为同一 tile 隐式创建替代 allocation。

## 限制

容量上界针对受控分配，不代表 RSS、线程栈或不透明 driver 内存。没有 spill、eviction、重算或 storage donation。整 block admission 可能拒绝较小内存串行调度可完成的程序。原型验证抽象字节预算和 hold，不验证实际设备或操作系统分配行为。
