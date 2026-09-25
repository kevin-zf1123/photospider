---
spec_schema_version: 1
id: PTH-03A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-03A：Core/Bezier长度与单调表

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-03 弧长与可证表](PTH-03_arc_length_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- path.bezier_arc_length_v1_strict
  - path.bezier_arc_length_v1_accelerated_apple_silicon
  - path.bezier_arc_length_v1_accelerated_x86_64

## 3. 端口与输出推断

输入PathSet CoreVerbs或纯Bezier primitives；输出length[1] Float64及table ArcLengthTableV1 Result。

## 4. 参数及合法域

epsilon_length_px>0默认1e-6；max_depth24、max_table_rows1048576建议；表需schema审查；不提供Float32 length。

## 5. 数学参考与舍入

每段s(t)=∫0^t||B′||；全长按子路径顺序相加，跳跃不计，Z计闭合线。表按精确dyadic二分：各原段分配epsilon/S；leaf chord下界/控制折线上界之差≤该leaf预算则接收，否则左右各分一半预算；DFS左先。存leaf端点t与累积s的向下/向上RN64界。表totals.length和独立length为同一RN64真长度，需在建表之外继续refine至单一舍入cell。

## 6. 实现成本与资源

基准O(leaves*d²)，堆栈O(depth)、表O(leaves)；exact sqrt包围，零段length0且表至少t0/1（M-only用单point特例）；耗尽预算明确失败。

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

(0,0)→(3,4)长5；100px直线任意细分长100；共线回折长度大于端点距离；空path length0+空表；长度区间包含独立高精度积分。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S02](../research-sources.md#s02)、[S17](../research-sources.md#s17)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
