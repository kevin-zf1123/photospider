---
spec_schema_version: 1
id: PTH-01A
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
source_maturity: D1_draft
revised_on: 2026-09-26
---

# PTH-01A：CoreVerbs路径构造

本具体规格已按[英文权威决策D01–D12](../decisions.md)修订。数学、端口与验收目标见本文；
未实现，候选键/新schema及证明算法在[技术门禁](../freeze-gates.md)完成前不得宣称已注册。
D1/D2仅保留来源成熟度，不表示首批顺序。

## 1. 范围与身份

所属族：[PTH-01 PathSet构造、分组与方向](PTH-01_construction_contract.md)。本成员与同族其他成员不能隐式互换。
继承[GEN-common](GEN_common_contract.md)、[PTH-geometry](PTH_geometry_contract.md)；精度、颜色、结构表示及执行条款通过这些文件继承01/02。
来源为上传草案，经D01–D12修订；规格不代表运行时已实现。

没有商业软件黑盒bit identity或未核验GPU支持声明。

来源候选键（全部需含版本；未冻结，不是可调用API）：

- path.make_core_path_v1_strict

## 3. 端口与输出推断

依序输入verbs Int64-width记录、control_offsets Int64、controls[N,2] Float64、subpath_offsets Int64、closed UInt8；输出path PathSet Result(CoreVerbs)。输入为既有schema可读字段，空rows使用Result字段而非零extent Value。

## 4. 参数及合法域

coordinate_system=pixel-xy-right-down；max_segments/max_controls沿schema；可选属性必须作为完整关联字段集输入，无默认外部owner。

## 5. 数学参考与舍入

逐字段按既有M/L/Q/C/Z arity及offset规则验证，controls按位复制，不插值、不删除零长段；M起点、terminal Z和closed一致。空数据仅接受标准空canonical字段。

## 6. 实现成本与资源

O(verbs+controls+attrs)校验/复制；新ObjectId与所有primitive/attribute关联在发布时重建；构造不允许部分前缀观察。

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

M(0,0),L(3,4)保留两点；M-only合法；坏arity/offset/NaN/association失败；closed与Z矛盾拒绝。

还须验证全域与非零ROI同位、真实读取/计算范围、合法负/零stride、独立/joint输出、
低预算、取消、cache-off、参数和资源版本变化、关联验证及保留owner寿命。

## 11. 公开执行与证据边界

实现验收必须通过WorkflowDocument→Compiler→ExecutionContext公开入口及独立数学参考。
本文样例是预期结果，不是已运行记录；附件oracle及其旧通过报告不验证修订后行为。
随机成员使用新版Philox规范和新的独立向量；技术门禁未清除前不发布golden。

## 12. 依赖

来源：[S02](../research-sources.md#s02)。
继承本族契约、GEN-common及适用的NOI-random/PTH-geometry。正式字段和算法门禁不得由示例替代。
