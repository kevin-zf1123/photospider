---
spec_schema_version: 1
id: GEN-common
kind: shared_operator_contract
category: 03-generation
status: AcceptedDesign
document_maturity: D1_draft
implementation_status: not_implemented
registration_status: technical_freeze_gates
inspection_date: 2026-09-25
inspection_source: user_uploaded_archive
inspection_package_version: 0.24.0
---

# GEN / NOI / PTH：共享定稿契约

## 1. 状态、继承与边界

本轮全部规格随已接受D01–D12修订，尚未实现；[英文决策](../decisions.md)为权威。
旧field.constant/field.coordinate测试实现退休，不保留兼容键。具体技术冻结事项见
[门禁](../freeze-gates.md)，不能用附件测试报告宣称实现完成。

必须继承 [NUM-common](../../01-numeric/op_specs/NUM_common_contract.md)、
[NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md)、
[FMT-common](../../02-format-color/op_specs/FMT_common_contract.md)、
[FMT-COLOR](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md) 和
[相对坐标尺度](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)。
01/02 已确定的数值、精度与颜色基线优先，不由本章放宽。对于基线未定义的新增03语义，
成员明确差异 > 本族几何/随机算法 > 本文件；显式中间RN阶段也必须在既定规则允许范围内列明。
本规格不重开已确定的数值、精度、颜色和错误枚举；01/02 中后发迁移目标优先于旧研究概述。

## 2. 数值与 profile

`RN_T(E)` 表示把精确表示的输入代入整个已指定实数/有理数表达式 E，最终仅舍入一次到 T；
除明确列出的中间 `RN64` 外，不允许把双精度循环求和冒充 strict。
默认 Float64；另可 Float32。整数、选择、计数、拓扑、分支和端点复制精确。
finite-only 几何/颜色输入拒绝 NaN/Inf；普通 constant raw tensor 允许相同 dtype 的按位复制。
默认 nearest/ties-to-even，渐进下溢，恢复调用者 fenv；禁用 fast-math/FTZ。
数学正零是 +0，显式端点/恒等复制保留原位（含 -0）；非零负值下溢保留 -0。

独立提议键后缀 `_v1_strict`、`_v1_accelerated_apple_silicon`、
`_v1_accelerated_x86_64`。不是mode参数或unsuffixed alias。部分成员提供GPU实际负载；GPU按后端独立profile。
同一算子/版本/profile输出逐位一致，改变合法输出需新版本；设备准入须验证。
离散/结构成员仅提议 strict；复合 workflow 不额外注册数学替身。
Float32 accelerated 与 strict 有序有限 IEEE 距离≤4；特殊值/零号单独检查。
Float64 **不窄化输入**；当 strict r 在正常有限 FP32 范围内：
`|a-r| <= 4 * 2^(floor(log2(|r|))-23)`，直接比较实际 Float64 值。
零、|r|<2^-126、|r|>0x1.fffffep127、分类不确定必须 strict。
许可是最终输出预算，不是每项/每 octave 4 ULP；支持、拓扑和统计抽样分支不能因加速改变。
加速候选须完整误差包络/证明 admission，不能仅凭本包样本自测准入。回退计数记诊断。

几何误差与数值误差是两件事：具名 flatten/smooth 版本可以近似原几何，
但 strict 仍须正确舍入它**明确选择的几何对象**的运算。
`epsilon_geom_px` 不能换成 `epsilon_numeric`；coverage tolerance 也不能从轮廓误差猜出。
证书须包含转换/flatten/求值/发布量化误差；没有界的高精度估计只能标 Measured。
没有达到界，返回 NotConverged/InvalidQuality，不把耗尽预算的近似当成功。

## 3. 坐标、shape、单位、类型

Canvas 的静态 H,W≥1，rank 1..8，每个 Value 的逻辑元素总数≤2^40，所有乘加检查溢出。
`origin_x/origin_y` 静态 Int64 默认0，像素为 `[ox+x,ox+x+1]×[oy+y,oy+y+1]`，
中心 `p=(ox+x+1/2,oy+y+1/2)`，x右、y下。顺序 HW/HWC；坐标分量固定 xy。
normalized_edge 坐标相对于画布为 `((x+1/2)/W,(y+1/2)/H)`；不使用 W-1/H-1。
原点进入 cache；请求 ROI 不能重新定义原点。大坐标用精确整数构造，浮点输出允许按 dtype
自然合并相邻像素，但离散索引/随机 counter 不从舍入后坐标反推。

除说明外浮点 Value 接受独立 Float32/64 输入，以精确表示值参与数学；没有隐式广播或 dtype 推断升级。
PathSet 控制点固定有限 Float64；DynamicPoints positions 固定有限 Float64，ID 为现有 Int64-width 记录。
不新增 Float16/UInt32/UInt64 等 runtime dtype。RNG 内部 uint64 不等于公开 dtype。
结构零 count 合法；普通 Value 仍不能有零 extent。通用字段不因 3/4 通道被称为 RGB/RGBA。

## 4. 颜色、alpha、coverage

完整 image 输出是 FMT 的 **straight** 多平面同尺寸 tensor；内部 alpha 在同一 tensor，角色显式，
不一定最后一通道，无 alpha 也合法。metadata 不携带外部 alpha owner。
需要颜色语义的生成器必须提供完整 model/primaries/white/transfer/reference/units/encoding；
草稿的构造器可显式写入 linear sRGB/D65，不能靠 C=3/4 猜测。
Lab/LCh 的 lightness 用 `l=L*/100`，a*/b*/C* 与 hue 单位沿用 FMT；禁止把旧 ColorArray v1 静默重解释。
无隐式 gamut/tone/transfer/quantization；先生成 Float32/64，再显式 FMT-06。

coverage 是像素面积比例 [0,1]，不是中心 SDF 的 smoothstep；SDF 单位 px，区域内负外正、边界+0。
coverage Float64 是通用 scalar coverage 描述目标，不能声称当前仅 Float32 的旧 mask facet 已接收。
透明端点和零 alpha 策略由颜色成员指定，raw numeric table 不建立 image 元数据。
metadata_mode=respect（构造器默认）/override/raw 仅用于消费已有颜色；新 image 源必须显式建立完整描述。

## 5. 端口、静态参数与输出推断

成员按列表顺序给出 Value/Result 端口。`canvas` 是静态参数组，不是未定义的输入对象；
几何向量/矩阵和颜色/表一般是动态 Value；seed/stream/frame/算法版本与预算是静态参数。
默认是构造器写入的建议值；直接节点仍提交全部 required 参数，未知字段 preflight 拒绝。
字符串、typed constants、表等复用现有参数 codec，不假装新 Python dict 已是 C ABI 参数。

Value 形状仅由输入 descriptor 和静态参数决定，不执行 producer 推断大小。
动态多段/点集合输出使用现有 Result/schema specialization/RuntimeCount；
新增 `ArcLengthTableV1` / `PathSamplesV1` 等为待完成正式审查的逻辑schema，需 review/公开实现，
不能以 JSON fixtures 宣称现有 representation.hpp 已支持。
CompleteBundle 校验关联后才发布；无成功前缀、无伪造 zero extent、无隐式对象快照。

## 6. 实际执行、Region、dirty

按成员数学依赖定义Regional、Halo或Whole，不能统一退回Whole。输出计算、输入读取、
完整验证和dirty分别声明。局部场只计算Q，相关filter读取必要halo，结构全局算法完整运行；
局部输出仍可依赖完整控制/路径/资源验证。全局坐标样本不受分块、线程和顺序影响。
无输入源无上游样本需求；所有控制与版本进入缓存身份。共享控制变更保守失效依赖区域。
空Q仅静态preflight，无payload读取或随机推进。Result完整封存后才可观察。
多输出仅运行所请求数学，必要共享typed validation不省略。GPU通过真实公开workflow验证。

## 7. 布局、返回映射、资源与寿命

接受合法负/零 stride、unaligned byte offset 和非零来源 origin；字段由现有访问器读取。
图像使用 FMT 同尺寸平面发布；不得把 HWC 逻辑轴当交错内存保证。
按所选执行规则materialize实际请求或完整Result；除结构按位复用的成员外 view 拒绝，不能 data-dependent 猜 identity。
返回 global Region、真实 owner、layout/stride 和 produced coverage；缺失不等于 ExplicitZero。
所有 outputs、scratch、堆/索引、动态 backing、validation I/O/work、resource snapshot 和 ancestry 计费。
work/stage/capacity 独立；managed peak 不承诺 RSS。cache-off 也必须能正确执行。

每成员给 O()，实际准入用 checked 字节/操作计数；大数组生成不允许 unaccounted 容器或私有线程池。
按像素行、细分、候选、积分或拓扑事件轮询取消；保留原 ResourceExhausted/Cancelled/Stale/upstream 状态。
不得以 strict fallback 隐藏资源失败。失败不发布该失败请求输出；独立已完成 output 保留宿主允许的结果。
保留值/Result/read window 在 context 销毁后仍可读；最后 consumer 释放时所有 owner/backing 终结。

## 8. 错误模型

| 条件 | 阶段 | 现有 Status | 范围 |
| --- | --- | --- | --- |
| 缺失/未知参数、非法静态半径/阈值/上限 | compile/preflight | InvalidArgument / InvalidDomain | Schema |
| shape/dtype/authority/schema 不支持 | compile/preflight | TypeMismatch / None | Schema |
| 动态 NaN/Inf、退化几何、负 width、乱序 stop | evaluation | InvalidArgument / InvalidDomain | 按实际demand的失败范围 |
| exact 最终数值无法有限表示 | evaluation | OperationFailed / ArithmeticOverflow | 按实际demand的失败范围 |
| 空集合上请求不存在的最近点等 | evaluation | OperationFailed / NoSolution | 按实际demand的失败范围 |
| 迭代上限仍无有效证明 | evaluation | OperationFailed / NotConverged | 按实际demand的失败范围 |
| 质量契约/拓扑验证失败 | evaluation | OperationFailed / InvalidQuality | 按实际demand的失败范围 |
| Result ObjectId/字段关系错误 | schema/publish | 宿主 InvalidAssociation 等原状态 | 保留 Association |
| 预算/取消/上游/不兼容 CPU | 原阶段 | NUM 原 Code/Reason | 不覆盖原 scope |

`InvalidRadius`/`FoldedMesh`/`RankTileMismatch` 等仅 bounded detail，不发明 FailureReason。
诊断至少标 operation/output、全局坐标或 segment/candidate ID、offending input、原始值位。

## 9. 最小实现验收

解析数值、独立 oracle、whole 与非零 ROI 投影、不同分块/线程顺序、特殊输入和边界、
低内存/work/stage、取消、cache-off、修改 seed/原点/资源重绑定、context 销毁后的 owner/read-window，
以及多输出单独/joint 等价均须测试。数学 oracle 自测不等于公开 kernel 工作流测试。
实际注册前必须增加 WorkflowDocument→Compiler→ExecutionContext 的公开 runner，
strict 比位、accelerated 比最终预算；本包没有伪造 runner 或 C++ 通过记录。

来源见[research-sources](../research-sources.md)，具体成员及族契约均在本目录。
旧附件oracle不作为修订后规格的运行证据。新增Result、随机序列与后端门禁见[freeze-gates](../freeze-gates.md)。
