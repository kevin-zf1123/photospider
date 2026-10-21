# 表达式与一维 LUT 算子

`numeric.sample_expression`、三个 `numeric.sample_expression_<profile>` key 和 `lut.apply_1d` 使用固定 CPU task。未带后缀的采样器可为 LUT 操作生成查询。每个选中输出 tile 有独立 dense 分配；preparation 在执行前确定精确 read slots 与 scratch。[英文文档](../Expression-and-LUT-Operations.md) 为权威契约。

## 未带后缀的表达式采样器

`numeric.sample_expression` 接收恰含一个 tensor 的 Result，schema ID／version／member key 可为任意合法值。Tensor 为无 batch 的 Float64 `[K]`，`1 <= K <= 256`。必填参数为 String `expression`、`[1,1048576]` 内的 Int64 `count`、Float64 `start` 和 Float64 `step`。Start 有限，step 有限且为正；count>1 时，`fma(count-1,step,start)` 须有限且大于 start。这些数值条件即使由静态参数给出也属于 unchecked 前提；preparation 检查参数类型及结构性 count。输出端口 `value` 使用 `photospider.tensor` v1/member `samples`，普通 Float32 `[count]`，不附着采样标签。

有界语法接受十进制／科学计数法字面量、`x`、`c[index]`、括号、一元符号、二元 `+ - * / ^`，一元 `abs`、`sqrt`、`exp`、`log`、`sin`、`cos`，二元 `min`、`max`。幂右结合且优先级高于一元符号：`-2^2=-4`、`2^-2=.25`、`2^3^2=512`，`0^0=1`。静态 parser 按 K 检查每个系数索引，拒绝其他标识符、错误函数调用、十六进制／NaN／Inf 字面量、超过 4096 字节的 source、超过 256 个 AST 节点或高度超过 32 的表达式。括号不增加节点。

Preparation 解析一次并持有不可变 AST。每个选中输出 tile 读取完整系数 tensor，包括 AST 未引用的项。Task 使用 4096 字节预分配 workspace，其中前 2048 字节容纳最多 256 个系数，后 2048 字节保存 postorder 中间值。它只计算所选坐标，使用全局 `fma(i,step,start)`，按源顺序执行 Float64 运算，最后转换为 Float32。基本运算和超越函数使用主机 Float64 实现；本 key 不承诺跨平台严格正确舍入。执行使用 nearest-even 和 gradual underflow。

系数、求值中间量及结果有限，且数学定义域合法，均为调用方前提。运行时不扫描系数、不校验除零／函数域，也不做最终有限范围检查；域外没有诊断保证。绑定替换复用 AST，各 Run 生成独立输出。Empty 无 task 或 payload 读取。复制系数时检查取消，AST 最多每 32 个节点检查一次，输出最多每 256 个样本检查一次。

## 带 profile 的具名系数采样器

带后缀族接受动态标量 start／end 和独立具名系数，必填静态参数为 `expression`、`coefficient_names`、`count`、`dtype`。Values 是普通 Float32/64 `[count]`，axis 是原子 Float64 `[3]`。每个选中 values tile 有固定 task，axis 有单独 task。Count 为一时排除 end payload、保留 schema；axis 排除所有系数 payload。所有 schema 仍校验。

其独立语法额外支持 pi／e、tan 和具名标识符，使用 `ln`，未带后缀 key 使用 `log`。Strict 对每个原语做 RN64 后传给父节点。Accelerated 四 lane 批次包围完整 strict 参考值，对不确定样本严格重算，保证最终 FP32-scaled 误差界。数值域和所选坐标精度是 unchecked 调用方前提。选中输出共享一个 IR call 的发布结果。

公开 helper 为 [`sample_expression_node`](../../../plugins/ops/include/photospider/ops/numeric/expression.hpp)，用于构造 WorkflowNode 并规范化准确的名称／连接映射。完整采样、数值和资源契约见 [NUM-01](../../built-in_ops/01-numeric/op_specs/NUM-01_sample_expression.md) 与[固定表达式执行](../../built-in_ops/01-numeric/expression-execution.md)。

## 线性 LUT application

`lut.apply_1d` 接收 Float32 query Result，以及至少两个样本的无 batch Float32 rank-one table Result。每个 Result 恰有一个 tensor，schema／member key 可为任意合法值。Query shape 可包含 batch axes 和多个通道。必填静态参数为 Float64 `sample_origin`、有限正 Float64 `sample_step` 和 String `out_of_domain`（`reject` 或 `clip`）。Origin 和 `fma(N-1,step,origin)` 须有限，后者须大于 origin，这些均为 unchecked 数值前提。输出 `value` 使用 `photospider.tensor` v1/member `samples`，保留 query 的 cell shape 与 batch axes；cell axes 为普通轴，不继承 spatial／atomic facets。

每个选中输出 tile 按 query 自身 grid 声明相交 query tiles，并声明全部 table tiles。运行时数据决定 table index，因此完整 table 是静态地址上界。实际算术只读取选中端点或相邻一对，不收集或数值扫描整表。多个 task 共享不可变输入 owner。每个 LUT invocation 使用零 scratch 和有界栈状态；`scratch_count=0` 时允许 scratch 数组为空指针。Empty 不声明 payload task。

Query 和实际消费的 table 值有限是调用方前提。`reject` 要求 query 在闭合 table domain 内，域外不保证诊断；`clip` 对有限越域查询返回最近端点。端点查询保留端点值及 signed zero。内部查询使用补偿 Float64 插值。整数转换所需的有限 `[0,2^53)` position 域及候选值有限可表示性均为 unchecked 前提；保留有界相邻 segment 修正，以及丢弃补偿不超过 Float32 ULP 八分之一的算法精度判定，无法确定插值精度时可返回 OperationFailed。支持 signed／HDR table 值，不赋予色域含义。

## 准备、所有权与错误

Preparation 检查静态 schema 和参数、canonical Host 输出、准确完整的 input tile 集合、空 task 参数和 scratch 大小。物理 view 遵循 Plan 已检查的 offset／stride 契约；raw 读取支持兼容的负／零 stride 与非对齐存储。运行时不创建 payload buffer。Root 在发射前准入所选输出、每 CPU lane 的最大 scratch 和执行器 metadata。稀疏 LUT query 仍需要完整声明的 table；稀疏 expression 只分配所选输出 tiles。

一个 call 的所有 task 共享发布结果。Source、资源、backend 和取消失败阻止成功发布，并保留操作状态类别。已接受工作排空后释放未发布存储。并发 Run 只共享不可变 preparation 和输入，输出分配与 expression scratch 各自独立。成功 Result owner 可存活超过 ExecutionContext。Invocation 恢复浮点模式和异常 flags。本路径不保证内容缓存或重复请求的结果身份。

语法或静态参数错误返回 InvalidArgument；dtype、shape 或 task signature 不兼容返回 TypeMismatch。输入／输出结构仍必须校验。数值前提违反没有诊断保证，LUT cell correction 和精度判定仍是可失败算术，地址数值域不校验。不添加自动样本 validator 或隐式布局转换。

## 行为验证

临时公开检查应将 sampler 算术与独立逐步参考比较，将 LUT 输出与精确有理插值比较。覆盖语法／AST／index 上限、全局索引、稀疏最大 count、完整系数／table 依赖、batch／atomic query 几何、端点／raw zero、取消和畸形 preparation。验证实际 sampler-to-LUT 组合、绑定替换、artifact 往返、并发 Run、低资源拒绝、零 scratch 空指针 ABI 调用，以及 context 销毁后的输出 owner。数值证据与平台性能结论分别报告。
