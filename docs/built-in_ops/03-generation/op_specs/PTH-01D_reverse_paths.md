---
spec_schema_version: 1
id: PTH-01D
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-01D：逐子路径方向反转

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-01 PathSet构造、分组与方向](PTH-01_construction_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- path.reverse_paths_v1_strict

## 3. 端口与输出推断

输入PathSet；输出PathSet同authority，子路径之间顺序不变。

## 4. 参数及合法域

attribute_policy；支持Core、Bezier、ArcSweep/FullTurn、Hermite、非周期spline；schema不支持的组合拒绝。

## 5. 数学参考与舍入

open从末点作为新M并逆段顺序；closed固定原M起点，对闭环逆序（原Z闭合线成为第一显式边，最后闭合用Z）。Bezier控制逆序；Hermite(P0,P1,d0,d1)→(P1,P0,-d1,-d0)；arc sweep反向且FullTurn显式direction取反；spline逆控制/weights并k_i→a+b-k_(m-i)。ArcLength查询在各subpath长度范围内镜像后重新排序，不对整PathSet简单u→1-u。

## 6. 实现成本与资源

O(rows)，未变字段可按位复制，导数求负按IEEE符号规则；arc/spline新增数值需RN64并重新验证连接，不可用epsilon吸附。

## 7. Demand、发布与dirty

目标执行规则：**Whole**。完整结构/全局算法输入与关联；完成全部所选Result后封存，不发布成功前缀。
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

reverse两次几何等价；closed原M保留；100px直线端点交换；global arc属性在多subpath下不能整列翻转。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S02](../research-sources.md#s02)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
