---
spec_schema_version: 1
id: PTH-07A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D2_draft
revised_on: 2026-09-26
---

# PTH-07A：逐子路径有向区间trim

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-07 按真实弧长裁段与虚线](PTH-07_trim_dash_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- path.trim_path_v1_strict

## 3. 端口与输出推断

输入path,start[1],end[1]；输出trimmed PathSet(CoreVerbs)。

## 4. 参数及合法域

units=normalized_arc/arc_px required；closed_wrap=false默认；normalized start/end∈[0,1]；arc_px按每子路径[0,L]校验；attribute_policy。

## 5. 数学参考与舍入

按每subpath实际长度，通常0≤a≤b≤L；a=b返回空，而不是全圈。closed且closed_wrap=true、a>b时取[a,L]再[0,b]，连接成一个open子path；open禁止wrap。恰a0,bL保留原closed标志与几何位。边界反求t*后用exact de Casteljau截取，发布新控制RN64；部分trim为open，消失的零长段不保留孤点。

宽度默认保留源位置：每个输出片段保存源subpath/snapshot及有序源弧长区间[a,b]。
在尚未发布舍入的截取几何上，片段弧长s对应原路径a+s；闭合wrap片段按[a,L]、[0,b]
两段分别映射，不能把连接后长度重新铺为原profile的[0,1]。输出控制点舍入后，须保留
截取参数到原参数的准确映射，不能假设新几何弧长仍精确等于原弧长差。
查询时先恢复源位置：逐子路径px域使用源s；逐子路径归一域使用源s/源L；
整PathSet归一域使用(源子路径前缀长度+源s)/源总长。三域均使用源绑定的原始长度和顺序。
附着width重建该绑定；独立width消费者使用同一源映射。需要把profile重新铺满每个片段时
必须显式rebind。正式Result必须能够表达并验证映射，否则默认reject_unmappable；
显式drop_unmappable仅删除无法映射属性并报告键/原因，不能改用未经批准的插值。

## 6. 实现成本与资源

O(segments+求逆迭代)，新控制舍入误差证书与连通性验证；无法同时满足连接/几何界时InvalidQuality。

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

100px线25..75得(25,0)→(75,0)；a=b空；闭环.75→.25 wrap；full-range bitidentity；同点不同参数回折不合并。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S02](../research-sources.md#s02)、[S03](../research-sources.md#s03)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
