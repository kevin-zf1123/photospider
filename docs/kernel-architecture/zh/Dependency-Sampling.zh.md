# 依赖采样算子

默认 registry 注册 `image.stmap`、`numeric.radius_gather` 和 `numeric.radius_scatter`，使用 dependency program version 1。Radius 操作通过当前 generic Value 路径执行，每次处理一个 sample。STMap helper 虽已注册，但当前 `OperationRegistry::start_dependency` 在 continuation 创建前拒绝其结构化 image 输入或推导出的 image 输出，返回 `TypeMismatch`。下文保留 helper 的采样规则及该执行边界。Dependency callback 使用请求级错误交付，不会将不同输出观察合成一个语义请求。输入依赖和控制证据使用[依赖数据与执行](Dependency-Data.zh.md)中的精确 fragment coverage。

## 范围、公开接口与执行边界

```cpp
using DependencyStart = std::function<Result<DependencyContinuation>(
    const DependencyQuery&, const BufferAllocator&)>;

struct OperationDefinition final {
  // Relevant staged fields; additional registry fields are omitted.
  std::string key;
  OperationTraits traits;
  OperationCallback callback;
  DependencyStart start_dependency = {};
  DependencyValidator validate_dependency = {};
};
```

注册记录持有 dependency start 函数，但公共 staged invocation 仅支持 generic radius 操作；结构化 image Value 会在 registry 路径拒绝，无法调用 STMap start 函数。类型声明省略 include 和其他注册字段。

## 数据模型与 STMap helper contract

Value dependency program 使用 `ValueFragments` 传输输入；它拒绝需要 `PlanarImage` 的结构化 image facets。因此 STMap helper 目前不能通过 registry execution 路径调用。这里记录参数、坐标、边界和 tap 规则供维护注册 helper 时参考。

`image.stmap(source, map)` 输入标准线性预乘 Float32 RGBA `source[Hs,Ws,4]` 及通用 Float64 `map[Ho,Wo,2]`，输出 Float32 `[Ho,Wo,4]`，保留 source 的 image-v2 facets。Source 各轴不超过 2^40。Map 分量 0 为源 x，分量 1 为源 y， 像素中心为 `i+0.5`。必填 String 参数 `boundary` 无隐式默认值：

| 值 | 长度 N 的轴上整数 tap i 的地址规则 |
| --- | --- |
| `constant` | 外部 tap 使用透明黑，不读取 source。 |
| `clamp` | 限制到 [0,N-1]。 |
| `wrap` | 模 N 的非负余数。 |
| `reflect` | 周期 2N，重复端点：p<N ? p : 2N-1-p。 |
| `mirror` | 周期 2N-2，不重复端点：p<N ? p : 2N-2-p。 |

N=1 时，除 constant 外均选坐标零。映射作用于全局源坐标，不对每个 fragment 单独处理边界。

对 helper 的四个有序 tap，令 $u$、$v$ 为 map 坐标，$l=\lfloor u-\tfrac12\rfloor$、$t=\lfloor v-\tfrac12\rfloor$。小数偏移、tap 权重和通道结果为：

$$
f_x = u-\tfrac12-l, \qquad f_y = v-\tfrac12-t, \qquad w_{dy,dx} = (dy ? f_y : 1-f_y)(dx ? f_x : 1-f_x),
$$
$$
y_c = \mathrm{Float32}\!\left(\sum_{dy=0}^{1}\sum_{dx=0}^{1} w_{dy,dx} \, s_{dy,dx,c}\right).
$$

总和按 C++ 实现做 Float64 顺序累加，dy 为外层、dx 为内层：左上、右上、左下、右下。constant 边界之外的 tap 按透明黑处理。这些公式描述注册 helper 的代码；结构化 image Value 仍不能进入其 callback。

每个输出像素先请求两个 map 分量作为 Control。它们必须有限且位于 [-2^40,2^40]。 程序计算 `floor(x-0.5)`、`floor(y-0.5)`，按左上、右上、左下、右下顺序访问 tap。 每个域内 tap 请求完整 RGBA，即使权重为零。Footprint 去重地址，数值计算保留全部四个 tap。读取并验证所有 tap 后，各通道按顺序执行 Float64 加权和，最后转为 Float32， 采用 nearest rounding 和 gradual underflow。透明 constant tap 仍保留 map/descriptor 证据。

注册 validator 在准备期检查 map rank、source 轴上限及 `boundary` 取值，包括 Empty 查询。Helper code 检查有限坐标和 source pixel，并为对应观察返回 `OperationFailed`；集合、状态、discovery 或分配超限返回 `ResourceExhausted`。这些规则描述 helper 实现，不构成可达的 image 执行契约；当前 Value fragment 或 registry guard 会在 poll 前拒绝结构化 image 输入和输出。

## 可执行的 radius 算法

两个算子输入通用 Float64 `source[N]` 与 Int64 `radius[N]`，1 <= N <= 2^40， 输出通用 Float64 `[N]`，不继承输入 facets。无参数。实际读取的 radius 必须在 [0,2^40]。

对于输出 o，`numeric.radius_gather` 包含满足 `abs(i-o) <= radius[o]` 的 i；读取输出位置的一个 radius，再对裁剪到源域内的区间求和。`numeric.radius_scatter` 包含满足 `abs(i-o) <= radius[i]` 的 i；每块最多检查 64 个候选，扫描全部 source radius， 保留包括被排除候选在内的完整 Control witness，随后只读取正命中的 Data。远端原本被排除的候选在 radius 修改后也能使输出 dirty。本实现无需可选空间反向索引。

两者按 source index 升序，从正零开始执行 Float64 左折叠加法。每个 poll 设置并恢复 nearest rounding 与 gradual underflow，调用者舍入模式不影响结果。非有限的命中 source、非法已观察 radius 或中间累加溢出返回 OperationFailed。分块不替换成块求和。 Gather 不验证自身输出观察之外的 radius；scatter 全控制扫描属于其声明的依赖语义。

固定 64 候选状态使用宿主 continuation lease。候选读取、阶段、证书和源数据均受执行限制。大型 scatter 的精确扫描超限时明确失败，不返回空证书或近似结果。

## 执行状态与错误

```text
Empty 查询 -> 静态校验 -> 不调用 state 或读取 sample
非空 radius 查询 -> start -> Control Need -> 精确供给
  gather -> 每块至多 64 个 Data -> 有序折叠 -> 发布 sample
  scatter -> 分块扫描全部 Control -> Data 命中 -> 有序折叠 -> 发布 sample
```

Coordinator 只向每轮 poll 提供声明的 Value fragment。Radius state 保存扫描游标、最多 64 个命中索引、阶段及 Float64 累加器。

## 校验、错误与行为证据

`OperationDefinition::validate_dependency` 是可选的纯 metadata/参数校验函数。 Compiler 在基础推导后调用；直接 session 在判断 Empty 前调用。它不能读取像素、 修改推导结果或访问 Q。不可变 definition 持有它，异常经过 fence。Empty 可跳过 state 构造，同时保持与非空查询一致的静态合法性规则。

`test_dependency_sampling` 通过当前 generic registry 路径执行两个 radius 操作，检查有限集合依赖关系、输出字节相同但 dependency edge 变化、control transpose、负坐标/端点、`N=1`、跨块求和和舍入模式不变性。[G4 radius workflow](../../../examples/g4_workflow/dynamic.cpp) 通过修改 radius、检查 gather/scatter 数值和 dirty evidence，并验证 frozen execution 保留旧输入，覆盖当前支持的路径。Workflow 没有可执行的 STMap 断言；registry guard 会在 helper callback 前拒绝结构化 image Value。

## 算法观察组与数值诊断

C++ `OperationOutputTraits::atomic_trailing_axes` 将完整尾轴分为同一原子观察， 适用于 CPU staged Atomic 输出；0 保持逐标量观察。通用 shape `{N,C}` 设置 1 时观察 shape 为 `{N}`；axis `{3}` 设置 1 时观察 shape 为 `{1}`。宿主将部分样本请求闭包为完整元组，统一计算、验证和出具证书，再返回请求样本的交集。 Image-v2 metadata 定义完整像素闭包，但当前 Value dependency executor 拒绝结构化 image plan。编译器边 metadata、structured bridge 和 joint 兼容性检查均保留分组，operation identity 包含该字段。版本 11 的 C descriptor 不暴露此 C++ trait。

`DependencyPhase::report_numeric` 接收经过检查的增量 `NumericDiagnostics`， 宿主以固定内联存储持有。报告包括实际 CPU profile、实现身份、计算值数量、strict fallback 及有界原因分类。后续失败仍保留已报告算术，体现在终态 atom progress 与 `OperationTiming::numeric`；structured consumer 累计每个上游观察。Cache hit 和未请求观察不增加算术计数。这些物理诊断不改变语义身份，也不能证明精度界。 见手动[数值 workflow](../../../examples/numeric_workflow/README.md)。

## 限制与调用方处理

Radius 操作使用通用 rank-one Float64 和 Int64 Value。带有结构化 image metadata 的 Value 使用 `PlanarImage` 路径；rank-3 generic numeric array 仍是普通 Value。`image.stmap` 尚无可执行的 planar dependency integration。不要以注册 helper 作为成功 workflow 证据。Radius scan 或精确集合超限返回 `ResourceExhausted`，观察到非法样本返回 `OperationFailed`，取消沿 execution host 传播。
