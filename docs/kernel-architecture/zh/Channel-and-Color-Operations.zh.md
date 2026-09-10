# 通道、alpha 与颜色算子

默认 registry 提供十个 CPU Whole 算子，使用 [ADR 0020](../../adr/0020-composable-operation-foundations.md)
中的共享 ABI/Traits 7 描述推断，覆盖公开 compile/execute、直接调用及 C contract 加载。
[英文实现说明](../Channel-and-Color-Operations.md) 为权威来源。输出为宿主分配的
非空 packed Value；输入按 byte offset、storage origin、signed/zero strides 读取，
不要求地址对齐。无隐式 resize、dtype 转换或 GPU 实现。

| Key | 输入 → 输出 | 必填静态参数与含义 |
| --- | --- | --- |
| `channel.extract` | typed Float32/64 Image/VectorField/ComplexField HWC → 同 dtype HW | Int64 `index`，0..63 且小于输入 C；输出保留所选 role/unit 的 ScalarField；图像 alpha 输出 canonical `coverage_semantics()`，可连接既有 mask 端口。 |
| `channel.merge` | 2..4 个同 dtype/shape 的 Float32/64 HW → HWC | String `semantic`，用 `semantic_parameter(target)` 生成。Target 为 Image（Float32，C=3/4）、VectorField（Float32/64，C=2/3）或 ComplexField（Float32/64，C=2）；输入为无 facet generic、ScalarField 或 coverage mask，已有 role/unit 必须逐通道匹配，name 不必相同。 |
| `channel.swizzle` | Float32/64 HWC → 指定 C 的 HWC | String `indices`，用 `channel_indices_parameter({2,1,0,3})` 生成，可重复，无隐式常量；语义按下述选择规则处理。 |
| `alpha.associate` | straight RGB Float32 HWC → coverage-premultiplied RGB | 无参数，RGB 乘 alpha，alpha=0 时输出零 RGB。 |
| `alpha.unassociate` | coverage-premultiplied RGB Float32 HWC → straight RGB | 无参数，对所有正 alpha 直接除法；零 alpha 返回零 RGB，无 epsilon。 |
| `color.assign` | Float32 HWC → 样本字节不变的 typed Image | String `semantic`，用 `semantic_parameter(target)` 生成；显式改变解释并验证目标样本域。 |
| `color.rgb_to_xyz`、`color.xyz_to_rgb` | linear sRGB ↔ XYZ Float32 HWC | 无参数，使用 `rgba_semantics().white` 的精确 canonical D65；association 必须 none 或 straight。 |
| `color.xyz_to_lab`、`color.lab_to_xyz` | XYZ ↔ Lab Float32 HWC | 无参数，保留输入显式声明的正 XYZ 参考白（Y=1）；支持 D65 和显式 D50，不进行适应。 |

列出的参数均必填，构造端显式选择。内建 merge 对 2..4 之外的输入数量在 callback
进入或 IR 发布前拒绝：直接调用返回 `InvalidArgument`，编译遵循既有数量检查返回
`TypeMismatch`。此边界只属于内建算子；自定义重复输入组
继续使用 1024 范围内独立声明的数量边界。Extract/merge/assign 仅建立推断出的 typed facet，
移除无关注释；带 opaque 解释的 generic merge 输入被拒绝。通用 numeric 运算可先移除
field 保证，随后显式 merge 建立目标并检查样本，因此可组合 extract→multiply→merge，
无须保留失效的 mask/image facet。

Swizzle 仅在选择结果仍可描述时保留变换后的 typed 元数据。完整 RGB↔BGR 排列保持
图像 profile；重复/缺失角色、alpha 不在末位、反转 complex 分量及其他无法表示的
选择输出无 facet generic HWC。Straight RGBA 去 alpha 可保持三通道 Image；
coverage-premultiplied RGBA 去 alpha 输出 generic，因为 RGB 仍乘了 alpha。
Swizzle 不进行 unassociate。Typed 源样本仍在消费前验证；generic 重排保持样本位模式。

Canonical indices String 为逗号分隔十进制，1..64 项，各项 0..63，最多 191 字节，
无空格、符号或前导零。公开 helper 负责生成/解析，调用方不需拼字符串。
`IndexListCount` 使用同一 parser 在编译期推断 C。闭集 semantic rules 描述
extract/swizzle/merge/alpha/color 变换，不按 operation key 特判。规则及参数进入既有
compiler/result-cache identity；C contract 使用相同枚举与推断，不引入任意 metadata
callback 或新的 WorkflowDocument 参数类型。

Alpha 算子保持通道顺序、白点、解释与 alpha bits（包括负零）。Associate 按定义丢失
straight 零 alpha 隐藏颜色，因此这种输入不能无损往返。正 subnormal alpha 在
nearest-even/gradual-underflow 环境直接参与除法，unassociate 溢出失败；完成后恢复
调用方浮点环境。

颜色转换按 role 读取前三个通道，支持 BGR 和重排 XYZ/Lab。输出颜色通道名/角色规范化
为 `R,G,B`、`X,Y,Z`、`L,a,b`，可选 alpha 名为 `A`，alpha bytes 原样复制。
RGB↔XYZ 使用 binary64 有理 sRGB/D65 矩阵；XYZ↔Lab 使用相对声明白点的 CIELAB
分段函数，阈值为 `216/24389`、`24389/27`，公式参考 [W3C 颜色转换](https://www.w3.org/TR/css-color-4/#color-conversion-code)。
Photospider 的显式白点契约不进行 CSS 的 D65↔D50 适应。保留 finite signed/HDR/超色域
结果，无 gamut clamp、transfer 编码、ICC/OCIO。转换到 Float32 前检查结果，非有限或
不可表示的结果带像素下标失败。

格式错误的参数/semantic payload 为 `InvalidArgument`；合法但不兼容的 model/
association/white、通道关系或 role/unit 在 callback 前失败（`TypeMismatch`，越界
selection 为 `InvalidArgument`）。非法单位/transfer 组合由 canonical semantic encoder
直接拒绝。Assign/merge 非法样本和数值溢出为 `OperationFailed`；取消/资源错误保留分类，
释放未发布输出，不返回部分成功结果。

## 可执行公开组合

[test_color_operations.cpp](../../../tests/integration/test_color_operations.cpp)
使用公开 Value、WorkflowDocument、Compiler、ExecutionContext。`graph()` 声明并
绑定输入，`composition()` 是最小 extract/process/merge：四个提取节点，红通道接
通用 numeric multiply，再用 `semantic_parameter(rgba_semantics())` 作为目标 merge。
修改倍率或在 merge 前连接其他 numeric 节点，绿、蓝、alpha 保持不变。同一源码也对
隔离安装的静态/共享包编译运行。

```sh
cmake --build build/issue257-static --target test_color_operations -j 8
ctest --test-dir build/issue257-static -R '^test_color_operations$' --output-on-failure
ctest --test-dir build/issue257-static -R '^test_installed_consumer$' --output-on-failure
```

将 build 路径替换为已有 shared 目录可运行相同公开 consumer。退出零检查独立预期：

- `[-2,.5,4,.5]` 红色乘二得到 `[-4,.5,4,.5]`，保留精确目标 RGBA facet；倍率一精确往返。
- 提取 alpha `.5,1` 连接既有 `mask.downsample_box`，结果 `.75`。
- RGB↔BGR 字节往返；重复索引正常执行并输出 generic；premul/straight 去 alpha 语义按约定区分。
- straight `[-2,.5,4,.5]` 关联为 `[-1,.25,2,.5]`；极小正 alpha 可使 `1,-1,0` 往返，零 alpha 隐藏颜色丢失，alpha bits 不变。
- linear red 的 XYZ 约为 `[.4123908,.2126390,.01933082]`；signed/HDR 往返。各自声明的 D65/D50 白点映射为 Lab `[100,0,0]`，黑色为零，signed XYZ 在测试容差内恢复。
- Merge 元数据公布 2..4 输入；合法 2/3/4 通道目标通过直接和编译调用执行，1/5 输入均不进入 callback。
- Float64 vector、Float32 complex 使用相同 merge/extract 契约；带 padding、非对齐、反转通道的 producer 读取正确。
- 非法语义、association、参数、role/unit、dense 分配边界和取消失败且不发布部分成功值。

Premul RGB 经 Lab 的组合为 `alpha.unassociate → color.rgb_to_xyz → color.xyz_to_lab
→ color.lab_to_xyz → color.xyz_to_rgb → alpha.associate`，全程 D65。D50 Lab 从显式
D50 XYZ 开始；与 RGB 转换之间需要独立显式白点适应，不在本次交付范围。
