# FMT-09 开发交接 / Codex review

交付日期：2026-09-25。基于用户提供的 `photospider-ops.zip` 与
`sleef-3.9.0.zip`；不升级 SLEEF，不引入运行时 Python/MPFR 依赖。
这是一份可构建、已做 Linux 本地验证的开发交付，**不是 macOS/FreeBSD
性能验收或全域正确舍入的形式化证明**。规范的 Proposed 状态不代表审核通过。

## 交付与应用

ZIP 的 `files/` 保存所有新增和修改文件的完整内容，路径相对原仓库根目录。
请先保留现有工作树，再把该目录覆盖到同一版本原仓库；这不是独立的完整仓库。
`changes.patch` 仅辅助审查，不能再重复应用到已覆盖的目录。
`MANIFEST.json` 记录原始文件 SHA-256、新文件 SHA-256 和变更类型；
`SHA256SUMS` 校验交付内容。`apply_overlay.py` 默认只核对基线，显式 `--apply`
才覆盖，已有不同修改时拒绝写入。原始两个压缩包及其摘要也列在清单中。
脚本校验所有待改文件后才写入；单文件原子替换，整批不是文件系统事务，
不要并发修改目标工作树。它允许重复应用，拒绝路径中的软链接。

```sh
# 在解压后的交付目录运行；第一次不产生修改。
python3 apply_overlay.py /path/to/photospider-ops
python3 apply_overlay.py /path/to/photospider-ops --apply
```

`validation/` 保存测试、基线复现、oracle 和 benchmark smoke 的实测日志。
这些日志不是目标机性能报告；源码内的 handoff 与 ZIP 根目录 HANDOFF.md
内容相同。已在原压缩包的独立解压副本上验证 dry-run 不写入、冲突拒绝、
完整应用、重复应用，以及 `changes.patch` 单独应用后逐文件摘要一致；
最终 ZIP 也做 CRC、路径与 SHA256SUMS 检查。完整覆盖文件共 30 个，
其中 13 个修改、17 个新增（含测试与文档）。

把用户提供的 SLEEF 3.9.0 源码解压到 `third_party/sleef/`，使其直接包含
`CMakeLists.txt`、`LICENSE.txt`、`src/`。该源码是原仓库已有构建前置条件，
本包不重复携带它，也不含编译产物或本地绝对路径软链接。

## 实现范围

注册六个原生 key：

```text
color.transfer_decode_strict
color.transfer_decode_accelerated_apple_silicon
color.transfer_decode_accelerated_x86_64
color.transfer_encode_strict
color.transfer_encode_accelerated_apple_silicon
color.transfer_encode_accelerated_x86_64
```

不存在无后缀自动派发别名。全部支持 Float32/Float64，十类曲线：linear、
power_gamma、sRGB、BT.709、BT.2020（smooth/rounded_10bit/rounded_12bit）、
BT.1886、PQ、HLG OETF、ACEScc、ACEScct。shape/dtype/轴不变；generic 支持
rank 1..8，图像使用现有 planar 存储。未实现 GPU、ICC/OCIO 引擎、色基转换、
OOTF、显示映射或存储量化，它们不属于 FMT-09 的本体职责。

支持 respect/override/raw、静态组/分量选择、精确局部 Data/Validation/dirty
映射、alpha/未选中分量位复制、generic identity view、planar raw identity
数据移动。semantic identity 仍验证被请求颜色，不能靠 view 绕过 NaN 检查。
planar semantic 的强制 `layout=view` 当前明确失败；auto 会物化并验证。

### 数值和算法优化

`transfer_program.hpp` 将完整实数公式编译成有界、不可变的表达式 DAG。
十进制常量为精确有理数，参数保留传入 Float64 位值；gamma 的倒数、HLG
派生常量和 BT.1886 黑白系数不提前舍入。分支用精确阈值的 binary64 上界
判定，Float32 提升后同样遵守精确阈值，而不是改用 RN32 阈值。

`transfer_math.hpp` 复用现有 NUM/CRV 的有界定向多精度内核：从 128 bit
逐级到 4096 bit，仅当区间两端舍入相同才发布严格结果。有理分支在必要时
回到精确有理运算，终端平方根走整数比值正确舍入；gamma=2 使用受控环境下
的一次硬件平方/平方根。线性、gamma=1、显式端点等有独立精确处理。
BT.2020 smooth 的 4096-bit 根隔离表有独立整数符号证书。

`transfer_fast.hpp` 是可关闭的区间过滤器，不是无条件用近似 pow 替代规范。
复用仓库的私有 SLEEF 3.9.0 U10 适配器，一组最多四个 binary64 样本；
x86 AVX2/FMA，Apple Silicon 使用现有 NEON 路径。分支按实际请求的样本
聚合；不会读取邻平面或 padding 凑满向量。误差在整个表达式传播：严格 key
仅接受确定同一舍入位的结果；accelerated key 仅接受满足最终输出误差界的
结果，否则回到严格算法。资源/取消错误不视为可恢复的数值回退。

用 `PHOTOSPIDER_TRANSFER_FAST_MATH=ON/OFF` 比较过滤器与参考回退路径。
OFF 不禁用别的算子的 SIMD，也不取消 gamma=2 的精确硬件特化。
没有实现额外的经验多项式、LUT 插值或 SME transfer 内核。

### 调度与数据访问优化

准备阶段解析元数据、构造曲线和稀疏映射；执行阶段不重复编译公式或构造
逐像素元数据。每个执行 callback 懒分配数学 workspace，复用常量区间，
以最多 64 个样本的 bounded run 处理，内部精确运算同样检查工作量与取消。
alpha-only 和 bit-copy 路径不创建数学 workspace。planar 使用现有 rectangle
接口保留行距/tile/ROI 边界；generic 使用实际 stride，支持负步长、零步长、
未对齐起点。外部 Workflow generic binding 仍要求 whole-dense；非标准
stride 的正确性通过直接 dependency/ValueFragments 接口测试，不放宽内核。

本次没有重写通用 scheduler 或增加跨调用数值缓存。小 ROI 的准备/发布开销、
混合 channel 的 generic 短 run、Float64 的高回退率仍是实机分析对象。

## 新的静态元数据表示

公共 `include/photospider/format/transfer.hpp` 提供
`TransferDefinition`、`encode_transfer_definition`、`decode_transfer_definition`。
沿用 tensor-description-v4 的 bounded `transfer` String 字段，完整记录规则：

```text
linear | srgb | bt709 | pq | hlg_oetf | acescc | acescct
fmt09-v1:power_gamma:<gamma 的 16 位小写十六进制 binary64 位模式>
fmt09-v1:bt2020:smooth
fmt09-v1:bt2020:rounded_10bit
fmt09-v1:bt2020:rounded_12bit
fmt09-v1:bt1886:<Lb 位模式>:<Lw 位模式>
```

调用公共编码函数生成字符串，不手写近似十进制。裸 `power_gamma`、`bt1886`、
`bt2020` 不足以构成元数据身份；参数里的 bt2020 省略 variant 则明确选 smooth。
显式 curve 构成完整断言，不会用源记录补全缺失 gamma 或黑白参数。源为
profile/config 标记时，必须已有完整一致的 analytic_binding；本体不会打开
外部配置。输出撤销不再成立的命名 profile 声明，保留显式解析出的色基信息。

semantic `group` 是唯一组名。RGB/Gray 支持，Gray 无 channel_axis 时只能
有单个 `indices={0}` 的 Gray 组且无 alpha。native reference 用
`scene_relative`、`display_relative` 或 `display_absolute`；相对 RGB 单位为
`relative`，相对 Gray 为 `relative_luminance`，绝对亮度为 `cd/m2`。这些字段
必须与曲线语义相容；没有隐式 0..255→0..1 或 cd/m² 归一化。

raw `components` 是 String：`all`，或例如 `0,2` 的唯一十进制索引列表，
最多 64 项。索引必须有明确的 `axis`（Int64），或由结构有效的输入元数据
提供 channel_axis；不接受负数、空项、前导零、重复索引或空白。raw 不能
同时指定 group。raw 保留原有元数据字节但不声称其仍是已验证颜色解释。
`metadata_override` 使用现有 tensor_description_to_parameter 编码。

简单 raw 例子：

```cpp
std::map<std::string, ps::ParameterValue> parameters = {
    {"metadata_mode", std::string("raw")},
    {"curve", std::string("power_gamma")},
    {"gamma", 2.0},
    {"components", std::string("0,1,2")},
    {"axis", std::int64_t{2}}};
// 绑定到 color.transfer_decode_strict：RGB 做有符号平方，alpha 位复制。
```

完整 compile/execute 和 planar 绑定范例见性能工具，测试包含 axis-free Gray、
respect 源记录解析和 override 的实际用例。

## 审查优先级

| 优先级 | 对象 | 重点 |
|---|---|---|
| P0 | transfer_program/math/constants、两个 oracle | 全实数表达式单次舍入；分支归属；signed zero；PQ plateau/E(0)；HLG 双精度端点溢出域；ACES floor/cap；极端参数消减与 4096-bit 上限。 |
| P0 | transfer_fast、现有 accelerated_math 与 photospider_sleef | SLEEF U10 在准入域的包围界、pow 对底数/指数的单调端点选择、最终误差界、AVX2/NEON 环境和尾部安全；不能仅凭 1724 个点认定证明完成。 |
| P0 | transfer_prepare 与 TensorDescription 公共校验 | 新 codec、单位词汇、profile binding 一致性；全重叠组更新、部分重叠完整组声明删除是否符合未来元数据策略；Lb 的 +0/-0 位身份有区别。 |
| P0 | transfer.cpp、依赖协议与 publication | 稀疏请求、无 peer backing、取消和资源耗尽的错误范围；失败不发布；64 样本批次的“多个潜在错误”优先级；host 资源统计是否完整。 |
| P1 | PlanarOperationInvocation / registry / execution | 新增 report_numeric 回调；多次报告合并和错误粘滞已测试；继续测并发、失败后的 diagnostics、allocation failure、TSan。C++ 头文件布局改变，必须全量重编译消费者；未改变 C 插件 ABI。 |
| P1 | 性能工具、ON/OFF 配置 | 计时与校验分离，核对真实 ISA、编译器、回退率、工作量预算；不能将共享 Linux 容器的 smoke 时间作为目标机吞吐结论。 |

严格内核的 4096-bit 上限是显式有界算法选择，未收敛返回
ResourceExhausted/CapacityLimit，不偷偷降精度。尚未穷举所有 binary32 输入，
也未证明每个有限 binary64 参数组合都会在该上限内收敛。需要重点补测 gamma
最小 subnormal/最大有限值、紧邻 1 的指数、相邻黑白亮度、极端 exponent
和难舍入点。数学区间论证与独立数值 oracle 是两层不同证据。

## 构建和正确性复核

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_TRANSFER_FAST_MATH=ON
cmake --build build --target test_transfer_math test_transfer_operations \
  photospider_transfer_performance -j4
ctest --test-dir build --output-on-failure -R '^test_transfer_(math|operations)$'
python3 tests/oracles/fmt09_constants.py --check
# 以下仅重新验证 fixture 时需要 mpmath；正常构建/CTest 不需要它。
python3 tests/oracles/fmt09_reference.py --check
```

浮点编译规则保留 `-fno-fast-math -frounding-math -ffp-contract=off`。
开启 unsafe math 后不再满足此交付的数值前提。Apple 的现有 NUM/FMT-06
SME 构建选项不属于本次 SIMD 路径，编译器不支持时按原工程的检查处理，
不要为 FMT-09 单独放宽全局 FP 规则。

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_ENABLE_ASAN=ON -DCMAKE_CXX_FLAGS=-fsanitize=undefined
cmake --build build-asan --target test_transfer_math test_transfer_operations -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure -R '^test_transfer_(math|operations)$'
```

大样本 MP 测试必须同时设置 `ExecutionOptions.maximum_dependency_work`
和 `ExecutionOptions.dependencies.maximum_work`。不要仅提高其中一个，
也不要通过不计费或把资源错误当数值回退来绕过预算。

## 本地验证结果与边界

环境：Linux x86_64，Clang 17.0.0，Release 主构建；使用上传的 SLEEF 3.9.0。
macOS/Apple Silicon NEON、FreeBSD、TSan、完整工程 CTest、所有 Float32
枚举及极端参数随机测试**未在此环境完成**。两个独立精度的 oracle 一致
只是数值证据，不等于形式化证明。

已完成的验证：

- `test_transfer_math`：1,724 个独立 golden 样例，严格参考路径逐位一致。
  ON 构建的 strict filter 接受 563 个、accelerated filter 接受 1,110 个
  非独立锚点结果，所有接受结果都通过对应误差界检查。
- `test_transfer_operations`：strict + 本机 x86 accelerated 共 3,448 次
  golden 比对；还覆盖六个注册 key、codec、元数据、axis-free Gray、
  respect/override/raw、精确 ROI、alpha-only、缺少 peer backing、rank 1..8、
  负/零/未对齐 stride、tile=128/256、轴置换、64-byte 行距、special values、
  FENV/FTZ/DAZ 恢复、依赖/取消/资源限制和 planar 数值诊断上报。
  最后另加 dtype/reference/encoding/source-state 的错误码与 reason 检查。
- 两个 Python oracle 的 `--check` 均通过；常量 oracle 包含 4096-bit
  BT.2020 根隔离的纯整数符号证书检查。正常 C++ 测试不依赖 mpmath。
- ASan + UBSan + leak detection 下，两项新增测试通过。
- 13 项所选 FMT/NUM/planar/public-interface 回归中，12 项通过；唯一失败
  为原有 `test_numeric_conversion` 的工作量断言，详见下一段。
- benchmark 的四组 smoke 均通过独立 golden 校验：generic sRGB、continuous
  PQ、tiled 3x3 HLG ROI 和 tiled ACEScc alpha-only。最后两组分别记录
  9 个颜色样本/72 字节源读数，以及 0 个颜色计算/16,900 个 alpha 位复制。
  smoke 只证明入口与计数可用，不据此宣称提速。

另外单独配置并完整构建 `build-off`，设置
`PHOTOSPIDER_TRANSFER_FAST_MATH=OFF`，两项 FMT-09 测试同样通过。
OFF 数学测试仍覆盖全部 1,724 个 golden，filter 接受数均为 0；实际
Workflow 仍覆盖 strict 与本机 x86 accelerated key。OFF 的 PQ continuous
Float64 smoke 也通过独立参考值校验，40 次严格回退／40 次 MP 调用；
ON 的对应 smoke 为 8／8。它们使用同一输入 palette，但运行时的共享
机器负载不同，因此这里只报告路径计数，不把计时比值作为算法提速结论。

### 保留并隔离的原始 FMT-06 回归失败

`tests/integration/test_numeric_conversion.cpp:910` 断言
`session->consumed_work() == 30 + (bad_at + 1) * 65` 失败。用原上传仓库内容
另外构建后，同一测试在同一位置失败。临时诊断副本仅打印该断言的实际值，
修改前后均为：`bad_at=0/1/17/63` 时实际 `98/163/1203/4193`，原期望
`95/160/1200/4190`，全部差 3。本交付**没有修改该原测试或 FMT-06 来掩盖失败**。
这些证据仅隔离该计数差异，不意味着整个工程已通过测试或 FMT-06 已被修复。
对应原始测试日志及诊断计数在 validation 中。

再次执行所选回归：

```sh
cmake --build build --target test_metadata_assignment test_channel_editing \
  test_channel_assembly test_channel_extraction test_numeric_conversion \
  test_transfer_operations test_transfer_math test_region_runs \
  test_data_movement_contract test_planar_preparation test_numeric_operations \
  test_planar_image_workflow test_planar_import -j4
ctest --test-dir build --output-on-failure \
  -R '^test_(metadata_assignment|channel_editing|channel_assembly|channel_extraction|numeric_conversion|transfer_operations|transfer_math|region_runs|data_movement_contract|planar_preparation|numeric_operations|planar_image_workflow|planar_import)$'
```

精度测试重点包含分段相邻浮点值、subnormal、HDR、负数和特殊值，但没有
覆盖所有 gamma/Lb/Lw 组合、所有物理布局与并发交错。不能把“未发现反例”
升级为“全输入无误差证明”。

## macOS / FreeBSD 实机性能工作单

先在每台机器通过正确性门槛，再采样。建立 ON/OFF × strict/本机 accelerated
× Float32/Float64 × encode/decode 的矩阵。先使用 256² 和小 ROI，再测
4096²；full/R-only/alpha-only 与 3x3 跨 tile 分别统计，tile=128/256，
continuous/tiled，workers=1/4/可用 CPU 数。补充真实图像分布，不只用内置
palette；单独测 toe、主幂函数段、ACES cap、PQ near-zero 及高回退数据。

每次记录：OS/CPU/Clang/构建选项、真实 ISA、编译与准备耗时、首轮和热轮
p50/p95、callback 总时间、invocation 数、evaluated/strict_fallbacks/
strict_math_calls、输入逻辑字节、输出 backing/virtual span、page fault、
RSS 和分配热点。先做单 worker 分解；并行 callback 总时间可能大于 wall
时间，不能直接相减当作调度成本。不要把 alpha-only 的零数学工作混入
颜色计算吞吐。generic 的 source_read_bytes=0 不意味着零读取。

macOS 可用 Instruments Time Profiler 定位 CPU 调用栈，结合分配分析检查
prepare、元数据验证、allocator、Footprint 运算、row/rectangle 访问和 MP
工作占比。Apple Silicon 要验证 NEON 被调用，检查 FPCR/FPSR 恢复及 subnormal；
本包没有在 Apple 机器上执行过。工具的 CPU 分析用途可参见 Apple 官方说明：
https://developer.apple.com/videos/play/wwdc2019/239/?time=495 。

FreeBSD 上检查 hwpmc 是否可用，先用本机 `pmcstat -L` 枚举事件；按实际
CPU 选 cycles/instructions/cache/branch 事件，不复制别的 CPU 事件名。
可用 `pmcstat -P <event> -O <log> <benchmark> ...` 采样，随后
`pmcstat -R <log> -G <callgraph>` 分析。权限、内核模块、虚拟化和可用事件由
实机环境确认。命令语义依据官方 pmcstat(8)，事件名是机器相关的：
https://man.freebsd.org/cgi/man.cgi?query=pmcstat&sektion=8 。

优先判断：Float64 strict 是否大量进入 MP（预期可能）；常量缓存是否因
分支切换失效；SIMD filter 的两端求值成本是否高于省掉的 MP；generic
固定 channel 短 run 是否限制向量利用率；小 ROI 是否主要耗在 prepare /
元数据 / publication；大图是否变成带宽/页分配限制。根据这些数据决定是否
继续增加曲线专用多项式或更大批次，而不是先引入未经误差界证明的近似。

交给 Codex 时要求输出：两平台构建与测试日志、失败最小复现、数值偏差分布、
ON/OFF 对照 CSV、调用栈与分配热点、ISA 证据和建议补丁。不要仅以“测试通过”
替代数值边界审查，也不要在没有相同正确性约束时比较两个算法的速度。
