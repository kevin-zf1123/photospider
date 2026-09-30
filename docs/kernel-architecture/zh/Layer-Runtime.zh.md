# Layer 值类型与 Result 边界

英文权威版本：[Layer-Runtime.md](../Layer-Runtime.md)。

Layer 值类型及纯算术 helper 仍属于 C++ API。Result coordinator 拒绝 `photospider.layer`、`photospider.layer_response`、`photospider.raw_rgba_sum`、`photospider.layer_contributions`、`photospider.weighted_layer_sum`、`photospider.optional_layer`，以及带 `photospider.layer` key 的 Result facet，因为这些打包字段不符合当前 planar 图像存储契约。图像使用[张量存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)。

## 1. 模块边界与所有权

`layer.hpp` 定义内存值契约和纯计算。`layer_operation.hpp` 仍声明旧分阶段操作的工厂，但这些操作的 Result 输出无法通过当前发布校验。Layer Result workflow 不属于受支持的图像存储路径。调用者仍可直接把 `LayerPixel` 传给值 helper，无需创建 Result。被拒绝的六个 ID 是 `photospider.layer`、`photospider.layer_response`、`photospider.raw_rgba_sum`、`photospider.layer_contributions`、`photospider.weighted_layer_sum` 和 `photospider.optional_layer`。

Planar image owner 保存图像样本，并让 view 保持其 backing。Layer 的 coverage/emission 成对字段没有 planar image owner 或 Region 契约。两种所有权模型不能通过附加 metadata 互换。

## 2. 核心数据结构与内存布局

```cpp
struct CoveragePixel final { std::array<float, 3> p{}; float a = 0; };
struct LayerPixel final { CoveragePixel coverage; std::array<float, 3> emission{}; };
struct LayerResponsePixel final { std::array<float, 3> q{}; float t = 1; };
struct RawRgbaSumPixel final { std::array<float, 3> p{}; float mass = 0; };
struct WeightedLayerSum final { std::array<double, 8> components{}; };
struct LayerContribution final { std::array<double, 8> components{}; };
struct OptionalLayerPixel final { bool valid = false; LayerPixel value; };

Result<SchemaTemplate> layer_schema(LayerRepresentation, const LayerSpec& = {});
Status validate_layer_result(const ResultRef&, const ResourceBudget&,
                             std::uint64_t maximum_window,
                             const CancellationToken& = {},
                             const std::function<Status(std::uint64_t)>& = {});
```

`CoveragePixel` 保存关联颜色 `P` 和 coverage `A`；每个 `P` 分量必须有限，`A` 必须有限且位于 `[0,1]`，`A=0` 时要求 `P=0`。`LayerPixel` 再加入有限、可带符号的独立 emission `E`。版本一算术使用线性 sRGB 原色、D65、relative scene 单位。Response 保存有限 `Q=P+E` 和位于 `[0,1]` 的 `T=1-A`，无法恢复原始 coverage/emission 分离状态。

Raw sum 保存有限 `P` 和有限、非负累加 mass `M`；`M=0` 时要求 `P=0`，mass 不是 transmittance。Contribution 保存 binary64 `P,A,E,w`，前七个值由 binary32 精确提升，`w` 必须有限且非负。已发布 weighted sum 保存 `Np[3],Na,Ne[3],W`，要求值有限、`W>=0`、`0<=Na<=W`、`Na=0 => Np=0`、`W=0 => 所有分子为零`。内部归约暂存可暂时违反关联关系。`valid=false` 的 Optional Layer 表示空的加权观察，不表示黑色。

## 3. 调度与状态机

```text
Layer values --纯 helper--> Layer / Response / RawSum values
      |                                  |
      +--旧 OperationDefinition --------+--> Result coordinator 拒绝 layer schema
PlanarImage -------------------------------> 受支持的图像存储与 Region 路径
```

Result 路径在 coordinator schema 校验处终止，不会到达完整发布。纯 helper 独立于该路径返回本地值，其错误仍由调用方处理。

## 4. 算法与数学

纯 helper 验证输入并返回新值，不修改输入。binary32 原语采用最近偶数舍入、渐进下溢、不收缩 FMA，并按规定步骤分别舍入。Helper 会恢复调用者的浮点环境。非有限结果和关联下溢会返回失败；helper 不发布部分值。

`layer_over(front,back)` 计算 `T=round32(1-Af)`，并分别舍入 `P=Pf+T*Pb`、`A=Af+T*Ab`、`E=Ef+T*Eb`。`layer_opacity` 要求 opacity 有限且在 `[0,1]`，只缩放 `P` 和 `A`，保持 `E`。`layer_emit` 要求 gain 和 emission 有限，且允许有符号数值。Front emission 加上 `gain*Eg`；behind emission 加上 `T*(gain*Eg)`。对显式不透明背景 `B` 做 flatten 时计算 `(P+E)+(1-A)*B`，并返回不透明 coverage。Response over 计算 `Qf+Tf*Qb` 和 `Tf*Tb`。

`raw_rgba_plus` 相加 `P` 与 `A`，并累加非负 mass。转换要么要求 `M<=1`，要么把 alpha 截到 `min(M,1)` 并保留 `P`；不会除以 mass。Weighted leaf 和 add helper 执行 binary64 算术，并验证每个已发布中间值。Finalize 将每个分子除以 `W`，舍入到 binary32，再验证完整 Layer。`W=0` 时不做除法，并返回 `valid=false`。

纯 helper `weighted_layer_leaf` 和 `weighted_layer_add` 不选择累加树。组合叶子的调用方负责选择并持有分组策略。

`SchemaTemplate::validate` 对六个 Result schema ID 和 `photospider.layer` facet 返回 `TypeMismatch`。`layer_schema` 可以构造字段描述，但当前描述无法通过 Result 校验边界。`validate_layer_result` 的可选 work hook 增加独立 work 上限；根 work 和 I/O 也会计费。

## 5. 限制与非目标

- `make_layer_operation` 可以构造定义，但这些定义不能恢复 Result 发布能力。
- Layer 不是 planar 图像存储，也不定义 planar Region 访问、分块图像执行或 GPU 后端。
- 该算术契约不提供认证数值误差界或进程 RSS 上界。
- 内存 helper 不做颜色空间转换，也不从样本值推断 working space。调用方需要处理关联无效、算术溢出和加权关联下溢等 helper 错误。
