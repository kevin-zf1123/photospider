---
spec_schema_version: 1
id: NOI-08A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-08A：有限open52驱动的Poisson量化

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[NOI-08 Poisson计数与电子shot模型](NOI-08_poisson_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[NOI-random](NOI_random_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- noise.poisson_icdf_v1_strict

## 3. 端口与输出推断

输入lambda[H,W,C] Float32/64，0≤lambda≤1,000,000建议范围；输出counts同shape Int64。

## 4. 参数及合法域

canvas维度须匹配，seed/stream/frame，domain8；max_count_search=2,000,000建议；C≤65536。

## 5. 数学参考与舍入

lambda=0输出0。否则u=open52，F(k;λ)=exp(-λ)Σj=0..k λ^j/j!；选最小k≥0满足F(k)≥u（相等归该k）。结果是有限u网格的Poisson quantile，而非无限连续源的理想PMF。λ非零有限二进有理时F与有理u的严格比较须可信区间，不能double累计到0或1后猜。

## 6. 实现成本与资源

基准递推仅适合小λ；大λ可用稳定CDF包围+bisection并认证。max_count_search内未证实答案NotConverged；不得裁到上限。

## 7. Demand、发布与dirty

目标执行规则：**Regional**。按Q计算输出；坐标/查询Value按所需样本读取。小型控制、stop/table、路径/mesh拓扑和资源描述完整验证；依赖全路径时可以完整读取路径，但不因此计算全画布。
完整控制/拓扑/资源改变保守失效全部依赖区域；逐样本输入改变按实际依赖映射失效。
空Q仅执行静态preflight，不读payload、不推进随机算法。global Region、origin、owner、layout
与produced coverage真实返回。多输出仅计算所请求数学，必要共享验证不省略。
具体callback及错误影响范围须按[GEN-common](GEN_common_contract.md)验证。

## 8. 数值、错误与生命周期

继承[GEN-common](GEN_common_contract.md)：strict完整式正确舍入，accelerated最终预算不按
tap/octave累计；同版本同profile位一致。图像平面存储、合法stride/offset、预算、取消、
关联及context后保留owner规则均适用。参数/schema错误、无解、未收敛、质量失败和宿主资源
错误保持区分；有限结果不可表示为ArithmeticOverflow。无效请求不发布成功前缀。

## 10. 成员验收

λ0全0；小λ阈值；λ1,u=.5→1；λ极大避免exp下溢算法错误；CDF(k-1)<u≤CDF(k)。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S19](../research-sources.md#s19)、[S21](../research-sources.md#s21)、[S05](../research-sources.md#s05)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
