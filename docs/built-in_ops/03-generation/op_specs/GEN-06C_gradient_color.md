---
spec_schema_version: 1
id: GEN-06C
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D1_draft
revised_on: 2026-09-26
---

# GEN-06C：模型特定语义颜色渐变

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[GEN-06 spread、数值表和语义颜色映射](GEN-06_gradient_lookup_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

不把旧CRV默认premultiplied输出沿用成新的image默认；可显式选择straight并完成描述迁移。

公开键按语义成员、算法版本及profile冻结；不存在未版本化兼容入口。

## 3. 端口与输出推断

输入t形状S、stops[K]、colors[K,C]；输出image S+[C]，当S=HW为图像。

## 4. 参数及合法域

model成员选择RGB/XYZ/CMYK/Lab/LCh/OKLab/OKLCh/HSL/YCbCr；严格递增stops，完整FMT描述；spread显式前置A；alpha策略仅由所选已定义成员授权。

## 5. 数学参考与舍入

调用对应CRV-06成员。RGB固定线性光插值：先D解码，Q=(1-w)*a0*D(c0)+w*a1*D(c1)，A=(1-w)a0+w*a1；A>0时straight RGB=E(Q/A)，A=0透明黑。端点/association underflow继承CRV。最终公开结果必须straight；Lab用l=L*/100。

## 6. 实现成本与资源

复合保留CRV严格舍入边界，非RGB不能机械按RGBA相乘；需要迁移的metadata先完成审查。

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

透明红与不透明蓝中点为straight蓝、alpha=.5而非紫；Lab/OKLab单位区分；色相跨缝遵照具名CRV。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S01](../research-sources.md#s01)、[S04](../research-sources.md#s04)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
