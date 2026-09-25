---
spec_schema_version: 1
id: NOI-07A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# NOI-07A：周期rank-tile消费

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[NOI-07 资源化rank场与阈值点集](NOI-07_blue_noise_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[NOI-random](NOI_random_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

公开键按语义成员、算法版本及profile冻结；不存在未版本化兼容入口。

## 3. 端口与输出推断

输入rank[Th,Tw] Int64和显式已固定resource descriptor；输出values[H,W] Float32/64。

## 4. 参数及合法域

canvas,offset_x/offset_y Int64默认0；Th*Tw=N≤2^40；无需随机seed，随机移位需另显式NOI-random→参数绑定workflow。

## 5. 数学参考与舍入

rank必须是0..N-1的完整排列。r=rank[(oy+y+offset_y) mod Th,(ox+x+offset_x) mod Tw]；输出RN_T((r+.5)/N)。这是rank midpoint而非NOI-01网格；若RN_T=1取nextDown_T(1)保持开区间，RN=0取nextUp_T(0)。通用rank入口仅要求结构/内容身份验证；具名blue变体还要求生产资源许可、来源和频谱质量批准。

## 6. 实现成本与资源

Whole校验rank排列O(N)及hash O(N)；验证完整资源后只计算输出Q。周期图不是无限非周期蓝噪声。

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

所有rank恰一次；负offset正确mod；hash/许可/质量descriptor缺失拒绝发布为blue_noise，可用raw_rank_fixture单独测试映射；示例不充生产golden。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S10](../research-sources.md#s10)、[S11](../research-sources.md#s11)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
