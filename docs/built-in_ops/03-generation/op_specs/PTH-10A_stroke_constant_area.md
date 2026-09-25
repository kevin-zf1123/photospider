---
spec_schema_version: 1
id: PTH-10A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-10A：恒宽Polyline描边面积

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-10 恒宽、变宽与轮廓输出](PTH-10_stroke_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- path.stroke_constant_area_v1_strict
  - path.stroke_constant_area_v1_accelerated_apple_silicon
  - path.stroke_constant_area_v1_accelerated_x86_64

## 3. 端口与输出推断

输入Polyline PathSet,width[1]≥0px；输出coverage[H,W] Float32/64。

## 4. 参数及合法域

canvas,dtype；cap=round/butt/square默认round；join=round/bevel/miter默认round；miter_limit≥1 required（默认构造4）；闭合忽略cap。

## 5. 数学参考与舍入

r=width/2精确。每非零segment侧带为法向±r的矩形；开放端按cap加半圆/不加/延长r的矩形；内折接合为各带union，外角按round扇形、bevel连接外侧点三角、miter延长外侧直线至交点且距离/r≤limit，否则bevel。180度回折round加半圆，bevel/miter不产生无穷尖角。零长subpath round为半径r圆，square为轴对齐边长2r方形，butt为空。coverage为完整union的真实像素面积一次RN。

## 6. 实现成本与资源

边界包含代数直线和圆弧，exact拓扑+certified积分/解析弧面积；不能用min片段coverage或累加代替union。miter阈值按exact判；资源受段/交点预算。

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

直线butt矩形面积；零点round圆、square方；width0全0；acute miter超limit落bevel；自重叠不得变深；小于1px宽按面积贡献。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S03](../research-sources.md#s03)、[S14](../research-sources.md#s14)、[S15](../research-sources.md#s15)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
