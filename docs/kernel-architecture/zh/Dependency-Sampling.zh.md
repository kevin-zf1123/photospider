# 依赖采样算子

默认 CPU registry 通过 version-one 依赖程序协议提供 `image.stmap`、
`numeric.radius_gather` 和 `numeric.radius_scatter`。每次 invocation 计算一个
通用 sample 或一个完整 image-v2 像素，返回请求级失败，不合批不同输出观察。
输入依赖和控制证据使用 [依赖数据与执行](Dependency-Data.zh.md) 的精确 fragment 协议。

## STMap

`image.stmap(source, map)` 输入标准线性预乘 Float32 RGBA `source[Hs,Ws,4]`
及通用 Float64 `map[Ho,Wo,2]`，输出 Float32 `[Ho,Wo,4]`，保留 source 的
image-v2 facets。Source 各轴不超过 2^40。Map 分量 0 为源 x，分量 1 为源 y，
像素中心为 `i+0.5`。必填 String 参数 `boundary` 无隐式默认值：

| 值 | 长度 N 的轴上整数 tap i 的地址规则 |
| --- | --- |
| `constant` | 外部 tap 使用透明黑，不读取 source。 |
| `clamp` | 限制到 [0,N-1]。 |
| `wrap` | 模 N 的非负余数。 |
| `reflect` | 周期 2N，重复端点：p<N ? p : 2N-1-p。 |
| `mirror` | 周期 2N-2，不重复端点：p<N ? p : 2N-2-p。 |

N=1 时，除 constant 外均选坐标零。映射作用于全局源坐标，不对每个 fragment 单独处理边界。

每个输出像素先请求两个 map 分量作为 Control。它们必须有限且位于 [-2^40,2^40]。
程序计算 `floor(x-0.5)`、`floor(y-0.5)`，按左上、右上、左下、右下顺序访问 tap。
每个域内 tap 请求完整 RGBA，即使权重为零。Footprint 去重地址，数值计算保留全部
四个 tap。读取并验证所有 tap 后，各通道按顺序执行 Float64 加权和，最后转为 Float32，
采用 nearest rounding 和 gradual underflow。透明 constant tap 仍保留 map/descriptor 证据。

非法 metadata 和未知 boundary 在编译期间被拒绝；直接 session 在构造 state 前拒绝，
Empty 查询亦然。
非法坐标或源像素使自己的输出观察返回 OperationFailed。集合、状态、发现工作量及分配
超限返回 ResourceExhausted。Empty 不读取 sample。输出 Region 和源 fragment 保持完整 C。

## 动态 radius 求和

两个算子输入通用 Float64 `source[N]` 与 Int64 `radius[N]`，1 <= N <= 2^40，
输出通用 Float64 `[N]`，不继承输入 facets。无参数。实际读取的 radius 必须在 [0,2^40]。

对于输出 o，`numeric.radius_gather` 包含满足 `abs(i-o) <= radius[o]` 的 i；读取
输出位置的一个 radius，再对裁剪到源域内的区间求和。`numeric.radius_scatter` 包含
满足 `abs(i-o) <= radius[i]` 的 i；每块最多检查 64 个候选，扫描全部 source radius，
保留包括被排除候选在内的完整 Control witness，随后只读取正命中的 Data。远端原本
被排除的候选在 radius 修改后也能使输出 dirty。本实现无需可选空间反向索引。

两者按 source index 升序，从正零开始执行 Float64 左折叠加法。每个 poll 设置并恢复
nearest rounding 与 gradual underflow，调用者舍入模式不影响结果。非有限的命中
source、非法已观察 radius 或中间累加溢出返回 OperationFailed。分块不替换成块求和。
Gather 不验证自身输出观察之外的 radius；scatter 全控制扫描属于其声明的依赖语义。

固定 64 候选状态使用宿主 continuation lease。候选读取、阶段、证书和源数据均受执行
限制。大型 scatter 的精确扫描超限时明确失败，不返回空证书或近似结果。

## 静态校验与可执行检查

`OperationDefinition::validate_dependency` 是可选的纯 metadata/参数校验函数。
Compiler 在基础推导后调用；直接 session 在判断 Empty 前调用。它不能读取像素、
修改推导结果或访问 Q。不可变 definition 持有它，异常经过 fence。Empty 可跳过 state
构造，同时保持与非空查询一致的静态合法性规则。

[G4 workflow](../../../examples/g4_workflow/README.md) 执行上游计算的 STMap 控制，
只读取源像素 {0,1023} 并检查恰好 32 字节；另通过 InputSnapshotStore 修改远端 radius，
并验证 FrozenExecution 保留旧结果。`test_dependency_sampling` 还检查有限集合 radius
关系、数值相同但边改变、控制 transpose、负坐标/端点/N=1、零权重 tap 验证、Empty
静态错误，以及舍入模式和跨块数值不变性。
