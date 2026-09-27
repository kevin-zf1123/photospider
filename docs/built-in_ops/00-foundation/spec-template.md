# 算子规格模板

## 文档组织与修订入口

`docs/built-in_ops/README.md` 是总导航；`01`～`12` 是功能类别，`00` 放公共约定，
`13`～`15` 放跨类别覆盖、路线与来源。
类别根目录下的中文主题页（例如 `01-numeric/core.md`、
`03-generation/generators.md`）是有内容的功能目录：说明适用范围、功能拆分、
关键输入输出与使用边界，并用稳定 ID 链接到具体规格。一个类别可以有多个主题页，
无须为了目录形式另建空的 `README.md`。新增或改名主题页时更新总导航。

| 要回答的问题 | 放置位置 | 修订时同步检查 |
| --- | --- | --- |
| 有哪些功能、如何选择、哪些仍是提议 | 类别根目录的中文主题页 | 总导航、对应规格链接和状态摘要 |
| 跨整个类别的目标语义 | `op_specs/<PREFIX>_common_contract.md`，仅在确有共享规则时创建 | 受影响的族契约及成员继承关系 |
| 一个编号族的共同端口、参数或数学规则 | `op_specs/<PREFIX>-NN_<topic>_contract.md` | 该族成员的例外与继承链接 |
| 独立算子或 workflow 的可验收契约 | `op_specs/<ID>_<function>.md`，例如 `NUM-04A_abs.md`；单成员也可直接用编号规格 | 所属主题页、当前实现入口及相关示例 |
| 当前注册、后端、限制和实测性能 | 类别根目录的 `implementation.md` 或明确限定范围的 `<topic>-implementation.md`、`<topic>-performance.md` | 公开实现文档、对应规格的实现状态和可运行示例 |
| 独立 oracle 与复现命令 | `oracle/ops/<category>/README.md` 及其代码；类别根目录可放 `oracle-coverage.md` 等覆盖索引 | 规格的验收链接与主题页入口 |
| 可执行 workflow 与复现命令 | `examples/<workflow>/README.md` 及其代码 | 规格的示例链接与主题页入口 |
| 跨类别的覆盖、路线或资料 | `13-coverage/`、`14-roadmap/`、`15-references/` | 受影响类别的入口链接 |

`op_specs/` 中的英文共享、族和成员规格是目标行为的权威定义；中文主题页须准确
概括其功能，不能另定端口、默认值或数值规则。继承顺序为成员明确规定、族契约、
类别共享契约、被引用的跨类别公共契约；成员必须指出例外。只有一个成员时无需
为形式完整性新建族契约；只有提议目录时无需创建空实现页、oracle 页或示例。
研究来源可集中到类别根目录的 `research-sources.md`，并由具体规格引用。

修订时按变化的范围选择落点：共享语义改共享契约，族规则改族契约，单个行为改成员
规格；随后更新实际受影响的中文目录摘要和导航。运行行为或注册状态变化时，再核对
代码、`docs/kernel-architecture/` 的当前实现事实、类别实现说明以及可运行示例。
规格状态、实现状态、oracle 覆盖和后端支持分别表述；`Accepted` 不表示已注册，
oracle 通过也不表示 runtime 已实现。文档只描述当前功能或当前目标契约，修订历史
留在 commit message，不在正文维护按日期累积的进度流水账。

## 规格对象

每个独立节点规格放在所属分类的 `op_specs/<ID>_<function>.md`，例如已有 PNT-05A。先标明对象是基础算子、复合 workflow、宿主交互功能还是外部模型适配。采用稳定需求 ID 和提议操作名；分别记录规格状态 `Proposed/Accepted`、实现状态（未实现/已实现子集/已实现）、D1/D2/D3、所属分类和依赖；给核对分支/提交和实现入口，不能用文档成熟度代替交付状态。已经实现的名称单独引用现有文档，不为拟定接口编造可运行示例。

一个完整实现规格应包含以下内容。分类文档可让一组算子继承公共约定，再用表格列出差异；存在例外必须明写。

| 项目 | 必填内容 | 典型错误 |
| --- | --- | --- |
| 目的与使用面 | 解决什么问题；输入从哪里来、输出给谁；典型工作流 | 只抄软件菜单名称 |
| 对应与区别 | 标准/软件功能、近似替代、同义词、不可替代之处 | 把 mean 与 median 合并 |
| 端口 | 默认 registry 或显式 factory；每个输入/输出名称与顺序、Value/Result、shape/schema、dtype、单位、颜色/alpha/emission、可选输入 | 将 RGB 曲线称为三维 LUT |
| 参数 | 类型、范围、建议默认、静态还是运行时、相互约束 | 半径没有 px/mm 区别 |
| 输出推断 | 静态多输出的 shape/extent/facet；Result schema/RuntimeCount/关联、空 rows、descriptor 与 finality | 使用未实现的动态 shape 推断 |
| 数学语义 | 公式、坐标方向、端点、颜色域、归一化、精度 | 只写“使用 OpenCV” |
| 算法 | CPU 参考、优化方案、复杂度、临时内存、并行与取消点 | 将近似算法标成精确 |
| 支持矩阵 | 通道、dtype、HDR/负值、CPU/GPU、平面/交错 | “支持所有格式” |
| 空间/时间需求 | 按输出/输入端口的 data/control/validation/descriptor demand、halo/边界、Exact/Conservative/Unknown、dirty 正向映射 | 把任意变换当 Elementwise |
| 返回映射 | 输出 Region、坐标原点、layout/stride、owner、缺失/ExplicitZero、发布范围与关联完整性 | 将 nominal plane origin 当实际 source support |
| 资源与寿命 | output/scratch/state/page/临时 backing/校验成本、同时存活 ancestry、root capacity/work/stage、cache-off、取消与最终释放 | 将 managed peak 当 RSS 上界 |
| 错误模型 | 除零/NaN/Inf/overflow/association underflow，发生阶段、Status code/reason/detail/scope、前缀是否保留 | 每次 silently clamp |
| 数值质量 | 舍入/累加顺序、容差、Measured/CertifiedBound 和支持范围，优化后端/fallback 的实际语义 | 用 residual=0 代替误差证明 |
| 验收 | 独立 oracle、解析小样本、容差、边界、最小公开工作流 | 只检查“输出看起来正确” |
| 来源与开放项 | 一手证据、软件版本/设置、未决问题、兼容等级 | 用旧对话证明商业实现 |

## 建议支持矩阵格式

| 项 | 参考实现 | 优化实现 | 未承诺 |
| --- | --- | --- | --- |
| 类型 | 明确，例如 Float32 输入、Float64 累加 | 例如 Metal Float32，有误差界 | fp16/fp64 GPU 未核验 |
| 通道 | 1/3/4/任意 C，列语义 | 同参考或声明子集 | 不能由 C=4 推断 alpha |
| 空间 | Whole / staged Value / paged Result，写明保证等级 | tile+halo、分块归约 | 更复杂映射须专用证明与实现 |
| 数值 | finite、signed、HDR、超范围策略 | 保留或回退 | 商业 bit identity |
| 状态 | 静态决定性输入与显式 seed | 并行确定性等级 | 隐式时钟/随机种子 |

## 验收模板

- 解析样本：列输入数值与预期输出，至少覆盖 identity 和一个非平凡案例。
- 数值 oracle：与独立公式或参考实现比较，注明绝对/相对误差、统计误差或边界误差；容差必须符合测量目标。
- 空间一致性：whole、nonzero ROI、跨 tile 边界；只有局部算法才要求 tile 行为。
- 语义：透明边缘、HDR、signed 和颜色域按算子需要选择，避免没有意义的全套矩阵。
- 工作流：通过当前公开入口执行至少一次，给输入、输出检查方法和实际命令；尚未实现的流程只能标概念 DAG。
- 资源/寿命：按节点风险验证低容量/work/stage、页窗口不足、取消、cache-off、多消费者、context 销毁后结果与 read window 存活、最终释放；Whole 节点不得声称分页执行。
- 多输出/结果：验证独立请求与 joint 等价、无关输出不读取；动态/空 count、ObjectId 关联、发布前验证、不可变前缀和 descriptor support。
- 性能：指定尺寸、半径、dtype、后端、硬件、managed peak、耗时统计与质量等级；列明计费排除项。

## 商业兼容等级

`S` 为已公开标准公式；`F` 为官方描述确认功能存在；`B` 为指定产品版本和设置的黑盒结果；`U` 为未核验。F 不推导 B。黑盒差异应固定颜色 profile、传递函数、位深、alpha、图层顺序、mask、是否 linear blending、缩放、CPU/GPU 和输出编码。

当前可直接继承 package 0.10.0 / C ABI 9 的[公共契约](contracts.md)；新节点只补差异。D2 转为 D1 前必须清除影响可观测结果的未决项。涉及公共数据类型、ABI、缓存身份或输入需求的选择，应走仓库现有 reviewed 流程；本研究不直接接受这些契约。
