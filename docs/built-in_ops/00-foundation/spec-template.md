# 算子规格模板

## 文档组织与修订入口

`docs/built-in_ops/README.md` 是总导航；`01`～`12` 是功能类别，`00` 放公共约定，`13`～`15` 放跨类别覆盖、路线与来源。

类别根目录下的中文主题页（例如 `01-numeric/core.md`、`04-mask-morphology/masks.md`）是该类别的入口目录，只包含以下内容：

1. 一句范围说明，指明英文规格是权威定义。
2. 基本术语：读懂族目录所需的类型、坐标、表示与 profile 定义。
3. 基础数值规定：继承的数值契约、unchecked 前提和跨族共同的语义区别，以链接为主。
4. 族目录：族 ID、功能、成员与族契约链接。
5. 逐成员入口：成员 ID、类别与成熟度、注册 key、实现状态。
6. 相关入口：研究资料、oracle 与相邻主题页。

一个类别可以有多个主题页，无须为了目录形式另建空的 `README.md`。新增或改名主题页时更新总导航。逐成员入口表的数据来自规格 frontmatter 的 `id`、`kind`、`document_maturity`、`*_operation_keys` 与 `implementation_status`。

| 要回答的问题 | 放置位置 | 修订时同步检查 |
| --- | --- | --- |
| 有哪些功能、如何选择、入口在哪里 | 类别根目录的中文主题页 | 总导航、规格链接与成员表 |
| 跨整个类别的目标语义 | `op_specs/<PREFIX>_common_contract.md`，仅在确有共享规则时创建 | 受影响的族契约及成员继承关系 |
| 一个编号族的共同端口、参数或数学规则 | `op_specs/<PREFIX>-NN_<topic>_contract.md` | 该族成员的例外与继承链接 |
| 独立算子或 workflow 的契约 | `op_specs/<ID>_<function>.md`，例如 `NUM-04A_abs.md`；单成员也可直接用编号规格 | 所属主题页的族目录与成员表 |
| 独立 oracle 与复现命令 | `oracle/ops/<category>/README.md` 及其代码；类别根目录可放 `oracle-coverage.md` 等覆盖索引 | 主题页的相关入口 |
| 跨类别的覆盖、路线或资料 | `13-coverage/`、`14-roadmap/`、`15-references/` | 受影响类别的入口链接 |
| 研究来源 | 类别根目录的 `research-sources.md`，由具体规格引用 | 引用编号 |

`op_specs/` 中的英文共享、族和成员规格是目标行为的权威定义；中文主题页准确概括其功能，不另定端口、默认值或数值规则。继承顺序为成员明确规定、族契约、类别共享契约、被引用的跨类别公共契约；成员必须指出例外。只有一个成员时无需为形式完整性新建族契约。

实现位置、后端与指令集选择、优化策略、资源用量、性能测量和验证运行记录不写入 `docs/built-in_ops/`。内核执行与资源规则见 [kernel 架构指南](../../kernel-architecture/README.md)，算子实现以代码与 Doxygen 为准。修订时按变化的范围选择落点：共享语义改共享契约，族规则改族契约，单个行为改成员规格，随后更新受影响的主题页。规格状态（`Proposed`/`Accepted`）、成熟度与实现状态分别记录在 frontmatter；`Accepted` 不表示已注册，oracle 通过也不表示 runtime 已实现。正文只描述当前目标契约，修订历史留在 commit message。

## 规格对象

每个独立节点规格放在所属类别的 `op_specs/<ID>_<function>.md`。frontmatter 记录稳定需求 ID、对象类别（基础算子、复合 workflow、SDK helper、外部引擎适配）、规格状态、成熟度、实现状态、父族、注册 key 和研究来源编号。

算子采用统一 unchecked 契约，见[公共契约](contracts.md#unchecked-执行边界)。schema/version、成员目录、dtype、rank、固定 extent、轴关系与参数类型在编译及静态准备阶段检查。运行时 count、offset、index、ID、关联、padding、编码规范及数值/几何域由调用方保证；生产者按定义构造结果，消费者不重复验证。静态参数的数值与几何前提同样 unchecked，只有决定静态结构的约束保留检查。规格保留算法本身的分支、正确舍入细化、候选选择和显式质量测量；非法 payload 的诊断不写成运行时必须失败的要求。

规格只包含以下内容。族契约可让一组成员继承共同内容，再用表格列出差异；存在例外必须明写。

| 节 | 必填内容 | 典型错误 |
| --- | --- | --- |
| 目的 | 解决什么问题；与相近算子或软件功能的区别 | 只抄软件菜单名称；把 mean 与 median 合并 |
| Interface | 注册 key 或公开 SDK helper；每个输入/输出的名称与顺序、Result 的 Tensor 成员、shape/schema、dtype、单位、颜色/alpha/emission 语义、可选输入；参数类型、范围、默认值或必填规则、静态还是运行时、相互约束；输出 shape 与 schema 推断 | 将 RGB 曲线称为三维 LUT；半径没有 px/mm 区别 |
| Mathematics | 公式、坐标方向、端点、边界扩展、归一化、单位；规范性的算法定义（迭代顺序、平局规则、停止判据、具名 profile）；数值类别、舍入阶段、精度保证与 NaN/Inf/有符号零/溢出行为 | 只写“使用 OpenCV”；将近似算法标成精确 |
| Behavior | 数学支持集（E、H(r)、W、R、S、T）；ROI/tile 与整图结果的关系；unchecked 前提；直通/copy 语义；合法输入下的结果状态（NoSolution、NotConverged、早停、质量门）；多输出之间的关系 | 把任意变换当 Elementwise；把数值前提写成运行时必须报错 |
| Acceptance | 解析样本、期望值、不变量与容差 | 只检查“输出看起来正确” |
| Conceptual DAG | 复合 workflow 的概念组合图（可选） | 把概念图写成已有 API |
| References | 一手来源、标准公式、兼容等级与开放项 | 用旧对话证明商业实现 |

## 验收模板

- 解析样本：列输入数值与预期输出，至少覆盖 identity 和一个非平凡案例。
- 数值容差：注明绝对/相对误差、统计误差或边界误差；容差符合测量目标。成员规定的精确舍入或逐位一致优先。
- 空间一致性：whole、非零 ROI 与跨 tile 位置的结果相同。
- 语义：透明边缘、HDR、signed 和颜色域按算子需要选择，避免没有意义的全套矩阵。
- 集合输出：静态容量下零与非零 count、ObjectId 关联、padding 与独立输出请求的等价性。

oracle 程序是独立的数学参考，保留在 `oracle/ops/`。修改或实现算子时现场编写执行检查，完成后删除临时测试程序、测试数据和注册入口；内核行为与包契约测试按[测试与验证](../../development/Testing-and-Validation.md)长期保留。规格中的验收场景表示需要满足的行为，不代表当前已有可执行程序。

## 商业兼容等级

`S` 为已公开标准公式；`F` 为官方描述确认功能存在；`B` 为指定产品版本和设置的黑盒结果；`U` 为未核验。F 不推导 B。黑盒差异应固定颜色 profile、传递函数、位深、alpha、图层顺序、mask、是否 linear blending、缩放和输出编码。

D2 转为 D1 前必须清除影响可观测结果的未决项。涉及公共数据类型、ABI、缓存身份或输入需求的选择走仓库现有 reviewed 流程，规格本身不接受这些契约。
