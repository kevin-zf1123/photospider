# 原子错误与数值质量

阶段 A #319 在 0.10.0 C++ 包中实现这些契约。安装消费方[示例](../../../examples/atom_outcomes_workflow/README.md)
运行真实区域 source、编译后的 DAG 回调和持有结果的 sink 观察。

## 坐标批次

`OperationTraits::joint_contract = 2` 接受 1..64 个不同 `AtomKey`。键由声明顺序中的
输出编号、1..8 维逻辑观察坐标组成；图像采用 HW 坐标，未使用的坐标槽为零。
contract 1 保留 ABI 9 按不同输出分组的协议。C++ outcome、progress、supply 和
pending-read 接口使用完整键。

每轮仅提供当前 Ready 成员，每个成员必须返回一个 Success、Need 或失败。
宿主在交付任何回复前检查整轮成员集合、payload 元数据、失败范围和质量附件。
未知、重复、遗漏、Waiting 或 Terminal 键都是协议错误。Need 挂起对应成员，
直到其精确授权输入范围被供给。每个成员只终结一次，局部失败不要求兄弟失败。
回调不得保留借用的 phase 或等待上游执行，状态与结果各自拥有已准入存储。

联合 phase 及借用函数对象保存在一个宿主准入的缓冲区中。准备阶段每成员只计一次
阶段，真实回调运行在浮点环境内，验证后逐个结束成员，不递归嵌套 Session::poll。
直接宿主可用 member_phase_bytes() 预算共享/成员 scratch 之外的适配空间。
没有 checkpoint 缓存时仍提供经检查的 miss 和纯 block 执行。

CPU 协调器收集至多 64 个已有需求坐标，支持同一输出的多个坐标，并在执行上游
读取前登记所有 Need。只去重完全相同的 source 传输，不将不同请求合并为新的
语义验证域。contract 2 的外层组失败不触发 singleton 重试。contract 1 保留
原有可选联合准入失败后的行为。

`ExecutionContext::execute_atoms` 为启用 managed resources 的 CPU Value 依赖计划
收集持有资源的 `AtomObservation`。命名输出必须是 Atomic/PerAtomOutcome。
`maximum_atom_observations` 默认值与硬上限均为 65536，同时受 root 容量和工作量
限制。空需求返回空观察集合。普通 `execute` 保持遇错即返回。结构化 Result 使用
既有三种发布策略；此接口不增加持久执行状态或 GPU 批次后端。

## 失败身份与范围

`Status.code` 是粗粒度分类，`reason` 是机器可读原因，`detail` 描述来源与范围。
诊断字符串不用于分支判断，Unspecified 表示旧接口仅提供错误码。

- Atom 指向生产者的一个逻辑观察。
- ValidationDomain 指向固定语义域；contract 2 当前接受完整不可变输出观察形状。
  域失败传播到覆盖的 Ready/Waiting 成员，禁止撤销此前已终结的语义观察，
  包括同一 Run 的更早物理批次。Run 保存失败全域以约束后续坐标。
  本轮请求子集不能定义验证域。
- Association 指向发布前验证的 Result ObjectId；观察者可见前检查关联字段。
- Group、Run、Waiter 描述操作影响范围，不在任意坐标编造语义失败值。

宿主将 node_id 或 input_id 绑定到真实失败来源，传播时保留该身份及原始 atom。
AtomObservation 的 output/key 指向请求消费方，因此可与上游原因的坐标不同。
source 传输失败保持 Io/Group 和原输入身份。失败没有成功 Value 或成功依赖证书。

读取、工作量和分配服务失败具有粘滞性，回调捕获异常或忽略错误后也不能成功发布。
已检测 Protocol 错误优先于后续取消。共享结果的等待者取消仍局限于该等待者；
已保存的 Protocol 终结可继续观察。错误成员集合或 payload 在本轮成功交付前拒绝。

ResultRef、结构化 Actor 和共享等待者以至多 256 字节内联诊断保存首个原因与范围。
宿主仅为同一原因补充缺失的生产者身份/范围，不覆盖已指定的上游身份。诊断复制
分配失败时保留固定机器字段并返回空字符串。内联空间计入所属 managed 对象。

## 质量证据

QualityReport 由命名工厂创建并持有不可变分配，调用方不能用枚举标志升级证据。
Measured 有限残差没有认证误差界。CertifiedBound 当前只用于经检查的整数对角
系统：1..4096 行、非零对角元、各输入绝对值不超过 2^26；快照名称是 1..256 个
可打印 ASCII 字节。报告保留全部精确整数证明行。

对保留系统取 R=max|a_i*x_i-b_i|、m=min|a_i|。乘积及残差是小于 2^53 的精确整数，
解的无穷范数误差不超过 R/m，残差舍入误差为零。正商按 binary64 最近舍入后向
正无穷移动一次；R=0 给出精确零界。证明不覆盖任意 Float64 系统、仅基于残差的
停止条件、其他范数或相对误差。

运行时附件当前要求无 facet 的 Float64 向量，维数匹配报告。成功原子的实际估计
必须精确匹配认证证明行，因此整数 2^24+1 的报告不能认证经 Float32 舍入为 2^24
的输出。Need 不携带质量。Domain 失败可携带 Measured，随原失败传播到下游；
它不描述消费方的新估计。成功变换不隐式继承其他算子的报告。可选结果缓存跳过
携带质量的值，拥有资源的执行中结果与返回值保留报告。

数值证据与依赖保证相互独立，Conservative 保持 Conservative。资源计账遵守
[已有范围](Managed-Resources.zh.md)；数值误差界和 Python 模型均不构成产品 RSS 硬上界。
