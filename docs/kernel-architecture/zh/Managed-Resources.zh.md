# 受控资源与必需临时存储

英文权威文档：[Managed resources](../Managed-Resources.md)。

`ExecutionContextConfig::managed_resources` 为现有受控 buffer reservation 和显式
临时存储启用同一个 `ResourceBudget`。`maximum_live_bytes` 保留为 payload 子限额。
`ExecutionContext::resource_budget()` 返回可共享根；它独立于 completed cache，
结果 lease 可以超过 context 的寿命。

## 容量与工作

容量向量分别限制 host、device、shared、metadata、referenced input、临时 disk、
entries、files、I/O slots 和 queue。host 包含 metadata/shared，device 包含 shared；
不能把重叠维度相加作为物理内存。整向量在 admission 前检查。lease 副本共享一个
owner；增长必须计算旧新容量共存，只有存储释放或尚未提交的预留取消后才能缩减。
cleanup 保护额度不能用于普通阶段。admission 不等待其他持有者，容量不足有限失败。

lease 对象容量自动计费，buffer、file 和 window owner 申报其 C++ 对象容量。
根启动、allocator control block/header、标准库私有分配、线程栈、驱动、OS page cache
及未接入 lease 的既有执行 metadata 均在此模型之外。不能据此认证进程 RSS 或原生
设备分配器开销。live 表示仍持有的已批准容量，含未使用 reservation；peak 是该
计数的实测峰值，不是完整输入类别的证明上界。保证限定为此模型的 WithinBudgetOrFail。

`reference(storage)` 按完整 caller allocation capacity 计入 Referenced 子限额，
同根按实际 owner 去重。返回 alias 保留 CpuStorage 地址，并让下游 view 保留引用
lease。启用根预算的 execute bindings 使用此机制；不推测 source callback 私有状态。

`consume(ResourceWork)` 原子预扣 work、bytes、requests 和 stages。已提交的工作
不因失败、fallback 或取消退款。singleton/joint dependency session 在执行前向当前
Run 根收费，包括 start 失败和 GPU discovery normalization。FootprintLimits 可携带
借用的根收费 callback，它不保存到不可变 Footprint 或语义身份。

## 临时 backing

TemporaryStorage 使用私有无缓冲临时文件和算术字节寻址，encoded extent 分别向
4096 字节取整。disk 统计 encoded file capacity，不代表 filesystem blocks 或物理
设备 I/O。无每页驻留目录、mmap 或 optional-cache eviction 依赖。read 显式检查范围
与窗口上限，不触发 producer；不可变 owning window 同时保留 backing 与根。

append 在写入前预留增长量；失败恢复旧 allocation end，回滚无法核验时保留 quarantine
额度。成功关闭文件可结算 quarantine；关闭失败继续计账。单调 frozen prefix 不可覆盖，
seal 禁止继续生产。prefix finality 和跨字段 association 属于结果发布者。取消阻止新
I/O，已提交同步 I/O 返回后才释放 owner；cleanup 不需要新窗口。

## 验证入口

英文页面列出 focused build/CTest 命令。test_resources 使用 16384 字节受控 host
限额处理 65536 字节临时 payload，检查独立计数、alias 寿命、并发 admission、输入
owner 去重、取消和有界失败。test_dependency_program 的 Need/上游/恢复例中，根
10000 只允许上游 6000 算法单位，根 30000 允许两个各 6000 的阶段；协议另计工作。
安装 consumer 目标 photospider_resource_consumer 仅通过已安装的 0.10 公共包运行。
