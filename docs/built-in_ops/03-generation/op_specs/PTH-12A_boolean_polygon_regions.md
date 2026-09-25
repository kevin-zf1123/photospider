---
spec_schema_version: 1
id: PTH-12A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-12A：拓扑检查的Float64多边形布尔

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-12 正规化布尔与欧氏区域offset](PTH-12_boolean_offset_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

公开键按语义成员、算法版本及profile冻结；不存在未版本化兼容入口。

## 3. 端口与输出推断

输入a,b Polyline PathSet；输出path PathSet(CoreVerbs)及geometry_report逻辑Result。

## 4. 参数及合法域

op=union/intersection/difference/xor required；fill_rule_a/b=nonzero/evenodd默认nonzero；epsilon_publication_px>0默认1e-9；max_events；attributes=drop_unmappable/reject_unmappable。

## 5. 数学参考与舍入

作为精确Boolean到Float64发布成员，等价于[PTH-12C](PTH-12C_boolean_exact_result.md)后接[PTH-12E](PTH-12E_publish_exact_geometry.md)。先对exact dyadic输入以有理arrangement求regularized区域集合操作，丢孤点/零面积边。规范化：外轮廓signed shoelace>0（y-down顺时针），孔<0；删除不影响形状的degree2共线点；每contour从lex(x,y)最小点开始，重复候选取全循环字典序最小，再按完整顶点序列排序。每exact顶点RN64一次，canonical零+0；publish后验证face/edge incidence、无新增交叉/合并且对应边参数距离≤epsilon。无法满足时InvalidQuality，不能snapping。

## 6. 实现成本与资源

exact arrangement通常O((E+I)log E)，保守naive O(E²)基准；交点/overlap事件数完整计费，operand swaps仅对交换律ops几何等价，不影响canonical输出。

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

矩形union/intersection/diff/xor解析面积；共享边与点接触regularization；孔洞orientation；亚ULP窄缝拒绝而非丢失；重复输入A∪A=A,A xor A=empty。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S14](../research-sources.md#s14)、[S15](../research-sources.md#s15)、[S16](../research-sources.md#s16)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
