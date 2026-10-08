# 测试与验证

本文说明当前 kernel package 的维护验证方式。Test 通过仓库 targets 和 installed consumers 检查可观察的软件行为。

## 构建前置依赖

配置检出的仓库或源码归档前，使用[文档中的命令](../../../third_party/SLEEF.md)
将固定版本的 SLEEF 3.9.0 源码下载到 `third_party/sleef/`。该目录由 Git 忽略，
离线构建也需要提前提供。CI 在配置前单独检出同一个上游提交；CMake 不执行下载。
使用已安装 Photospider 包的下游无需准备这项源码依赖。

## 开发循环

实现期间使用 scoped formatting/lint、affected target 与 focused test。任务明确要求
clean full validation 时，删除选定的 ignored build 目录，从头 configure，构建全部
注册目标并运行完整 CTest 清单。失败导致源码修改后，先重建受影响目标并重跑相关
测试，再针对最终源码状态重新执行完整 build 和 CTest。clean validation 不自动包含
sanitizer 或跨平台矩阵；native validation 不使用 Docker 或本地 architecture emulation。

## 必需行为领域

Kernel test 覆盖：

- WorkflowDocument 与 graph/IR/plan validation；
- typed stage identity 与 canonical digest 分离；
- 即使 operation key 相同也拒绝 cross-registry IR/plan；
- CPU compile-plan-execute 与可选 GPU selection/fallback；
- 多个独立 graph/execution context；
- 跨 deterministic CPU/GPU FIFO 共享的 single ExecutionContext-wide waiting-callback
  bound、worker pop 后的 capacity recovery，以及普通 mixed-lane concurrent Run；
- no-GPU fallback denial、waiting-admission rejection、backend queue rejection 与
  submission exception fallback 上的 first-failure priority：cancellation 先于 graph
  `Stale`，graph `Stale` 先于 original failure，同时不产生 stale result，并精确恢复
  waiting/in-flight/resource。Backend submit rejection 与 exception-fallback case
  使用 GPU-enabled context、显式 GPU physical plan、存在的 GPU lane 以及精确的
  `Backend::Gpu` hook consumption；它们证明 CPU callback 不会冒充 GPU path，且清除
  hook 后 GPU execution 恢复成功。另有独立 CPU queue-rejection case 保留 CPU 覆盖。
  独立的无 sleep cancellation 与 stale race 会占用唯一 CPU worker、阻塞目标 Run 的一个
  GPU callback，并且只让 external-stop `Status` construction 失败。目标 CPU callback
  进入 FIFO 后，另一个独立 CPU Run 会在 worker occupant 释放前把 successor sentinel
  排在它之后。Sentinel 完成证明目标 callback 已被 pop，并已完成 abandonment。此时 GPU
  callback 仍被阻塞，目标 future 必须保持未完成；释放 GPU callback 后，future 返回选中的
  code 与空 diagnostic。Cancellation 与 stale 两种 case 都会让全部 callback 完成退役，
  且同一个 context 可用 current snapshot 再次成功执行。另一个无 sleep queued-attempt
  regression 会占用唯一 CPU worker，并在目标 Run 完成 post-submit external-stop
  observation 后、仍持有 Run mutex 且尚未进入 condition-variable wait 时阻塞它。这证明
  single-node side-effecting/non-cacheable target 在 coordinator 最后一次检查后已经进入 CPU
  FIFO。随后替换 graph，再依次释放 Run gate 与 occupant 时，必须以零次 target entry 返回
  `Stale`；在同一 boundary 先 cancel 再替换必须以零次 entry 返回 `Cancelled`。两个 future
  都会收敛，且同一 execution context 可以成功执行重新编译的 current plan；
- bounded ready work 与 `ResourceLedger` settlement；
- cross-backend copy/backend label、cancellation、stale completion 与 exception fence。
  Private condition-variable barrier 只在 named Value、diagnostic、digest 与 execute timing
  全部完成后阻塞 nontrivial vector result；hook 后分别 cancellation、graph replacement 与
  两者同时发生时，依次返回 `Cancelled`、`Stale`、`Cancelled`，不发布
  `ExecutionResult`，并各自以一次健康 execute 证明精确 cleanup；
- Value/Region/strided-layout/facet/buffer 负向契约；
- operation/provider ABI version/size/alignment/pointer/count/bounds/lifetime，包括
  Result operation ABI 2 typed parameter schema、demand view，以及带精确 destroy/close count 的
  deterministic owner-allocation failure。Copy-aware C++ embedding 测试通过 rvalue 注册一个
  提供 Result continuation factory 的 callable，再将其设为拒绝后续 copy。Registry freeze 后，
  `start_result` 和 `poll` 发布 Float64 值 41；factory 恰好调用一次，callable copy 数不增加。
  带额外 input 的 Result query 返回 `InvalidArgument`，且不再次调用 factory。在另一个独立的未
  freeze registry 中加载合法 Result DSO 也成功，已注册 callable 不会被复制或调用，并检查 DSO
  加载的事务性。这些检查覆盖 immutable callable handle；
- `test_prepared_workspace` 覆盖 Whole 与 Dependency-v2 Result continuation。静态准备把 0 或
  4096 bytes 加到声明的 16-byte workspace 上；同一编译 plan 执行两次不会再次 prepare。UInt8
  输出分别为 0 和 17。4096-byte 情况在 2048-byte Payload 限额下返回 `ResourceExhausted`；额外
  workspace 请求导致总量溢出 `UINT64_MAX` 时，会在 preparation 阶段拒绝。成功与失败执行都会释放
  Root Payload allocation；
- `test_result_exceptions` 覆盖 direct 与 compiled Result scalar、contract-1 和 contract-2 的
  start/poll 边界。标准异常转换为带 `HostException` 的 `OperationFailed`，保留 `what()` 文本
  （为 null 时使用空文本）；`std::bad_alloc` 转为 `ResourceExhausted`，非标准异常转为带固定
  `HostException` 诊断的 `OperationFailed`。较早的 allocation failure 优先于随后异常；失败 joint poll
  会锁存错误且不再次调用 callback；抛异常的 failure observer 不能替换 producer 已选定的 failure；
  direct 与 workflow case 结束后 Root Payload 均归零；
- `test_backend_admission` 覆盖 Result 注册与启动。它拒绝没有任何受支持 backend 的 operation，以及
  没有 CPU target 的 CPU-fallback 声明。GPU-only operation 的 direct startup 和 CPU compilation 返回
  `BackendUnavailable`；Native-GPU plan 可编译，但 GPU-disabled context 会在进入 factory 前拒绝执行。
  GPU-targeted 且允许 CPU fallback 的双 backend operation 在该 context 中只进入 CPU factory，发布
  UInt8 值 17，并记录一个 fallback reason；GPU factory 调用数为零。
  `tests/consumer/CMakeLists.txt` 将相同源码注册为 `installed_result_exceptions` 和
  `installed_result_backend_admission`；
- `test_plugin_registry` 中的 Result invocation prevalidation：direct `start_result` 会在检查 malformed
  input metadata 前，对不支持的 GPU backend 返回 `BackendUnavailable`。首个或最后一个 input slot 缺少
  Result schema 时返回 `TypeMismatch`；fixture 的 `prepare_static` 会校验预期的单个 Float64 tensor 和
  shape。Metadata 被拒绝时不会进入 operation factory。Compiled callback 若返回 invalid Result、错误
  output tensor type 或不完整 tensor coverage，会以 `InvalidArgument` 和 Protocol detail 失败。其他
  进程内 Value API 仍是独立接口；
- `test_result_diagnostics` 覆盖 full-coverage zero-stride Result publication 的精确与饱和
  `computed_elements`。`test_plugin_registry` 也运行同一 shared fixture。两项 focused test 均已在本地通过；
  installed consumer 注册为 `installed_result_diagnostics`，该测试也已通过；
- native loading 前的精确 operation/provider library path validation：显式长度的合法
  fixture path 后接 embedded NUL 与 suffix 时返回 `InvalidArgument`，不发布 operation
  key/provider schema，且 owner-allocation、native-load 与 native-close test hook 均为零；
- 真实 DSO output-sink at-most-once enforcement：accepted-then-duplicate 与
  rejected-then-duplicate callback 分别记录精确 sink return `(1,0)` 与 `(0,0)`；success、
  backend unavailable、ordinary failure、unknown callback result 与 callback-reported
  cancellation 都成为相同且稳定的 terminal `OperationFailed`。Duplicate backend
  unavailable 的 CPU invocation 为零，且不发布 output；deterministic callback-held
  cancellation case 证明 host cancellation 仍有更高优先级。Null sink context 没有副作用，
  随后的合法 publication 成功；registry retirement 仍对 descriptor destroy 与 native
  lease close 各执行恰好一次；
- 真实 DSO dense fixed-shape 在 `INT64_MAX + 1` bytes 与 rank-two `{2, 2^62}` 的边界、
  紧邻两者上界的 rejection、通过 compile-safe helper 验证的 32-bit host-size
  representability，以及带精确 destroy/close count 的 transactional multi-descriptor
  rejection；
- semantic IR 前的 unknown/missing/wrong-type/conflict parameter rejection；
- side-effecting/non-cacheable operation 在 semantic、optimized 与 plan stage 中保持，
  并覆盖串行重复执行时的 callback 顺序、调用次数与不复用旧 result；
- Whole/Elementwise/Halo demand propagation 与 execution-time coverage；
- 带 named oracle 或显式 `unchecked` identity，且不生成 verdict/evidence output 的
  raw benchmark diagnostic；当 oracle 返回拒绝或抛出异常时，仍保留已完成的
  compile/plan/execute/operation/backend/digest observation，并独立报告 correctness。
  任一 iteration 的 execution 返回 `Cancelled` 时会中止整个 run，不发布 partial/
  success report。Runner 还会在 caller oracle 前后立即观察 cancellation，并在最终
  report-publication 线性化点再次观察。Oracle 不接收 token，不能被抢占；若它返回
  false 或抛出普通异常后观察到 cancellation，则 cancellation 优先于 oracle outcome；
  最终 publication 观察之后到达的 cancellation 不会撤销已返回 report。其他 execution
  failure 保留为 sample，后续 iteration 继续。若普通 oracle exception 的 `what()` 为
  null，则它会成为 reason 为空的 `OperationFailed` sample；已完成的 raw compilation/
  execution diagnostics 保持不变，后续 iteration 仍可成功。独立的 null-diagnostic
  cancellation case 证明 post-oracle cancellation fence 仍是顶层权威。由于 monotonic
  clock 可能只有微秒分辨率，duration 允许为零。Deterministic regression 覆盖另一线程
  在 oracle barrier 中取消、self-cancelling true/false/throwing oracle、逐字段检查的
  两次 iteration null-diagnostic-then-success sequence，以及通过 noninstalled
  test-kernel seam 暴露的无 oracle post-execute window。

Daemon test 位于 `photospider-daemon`，覆盖 local frame validation、本地方法 routing、
临时 Session/Job lifecycle、restart loss、multi-Session behavior、cancellation、
Session close、result release、shutdown 与隔离 installed-kernel boundary。

## Installed boundary

Package gate 将 Photospider 配置并安装到 fresh prefix，再只通过
`find_package(Photospider CONFIG REQUIRED)` 配置 external C/C++ consumer。CI 会为
默认 static kernel 与 `BUILD_SHARED_LIBS=ON` 都运行该 gate。它验证：

- consumer 源文件实际编译安装公共头；`operation_plugin.hpp` 是首个 include，
  并有 `element_type_value` 与 `noexcept` 编译期断言。完整头文件自包含扫描
  是单独手动检查，不属于此 CTest；
- linked C SDK compilation unit 会实际运行；downstream shared bridge 会链接
  `Photospider::kernel`、执行 C++ compile/execute pipeline，并由 consumer executable
  调用。因此默认 static archive 会作为 position-independent input 被真实 shared
  library 使用；
- `kernel`、`operation_sdk` 与 `data_provider_sdk` component discovery 精确导出
  `Photospider::kernel`、`Photospider::operation_sdk` 与
  `Photospider::data_provider_sdk`；两个 SDK target 都只包含 header。Configure-time
  property check 要求 kernel 与 operation SDK target 都携带 `cxx_std_17`，要求纯 C
  provider target 不携带该 C++ feature，并拒绝任何意外的 `data_definition_sdk`
  target；
- downstream bridge 与 final executable 自身只声明 `CXX_STANDARD=14`、
  `CXX_STANDARD_REQUIRED=ON` 与 `CXX_EXTENSIONS=OFF`，且都不私自请求 C++17。
  Linked imported kernel 与 operation SDK usage requirement 会把两者的实际 compilation
  提升为 C++17。两份 translation unit 都使用与 compiler 相符的 static assertion 验证
  结果：MSVC-compatible frontend 定义 `_MSVC_LANG` 时使用该宏，否则使用
  `__cplusplus`。Consumer 不私自添加 `/std:c++17`，也不要求
  `/Zc:__cplusplus`；dialect elevation 仍只来自 imported usage requirement。Linked
  pure-C SDK probe 保持显式 C11 translation unit，且不获得 C++ standard flag；
- 已移除的 `data_definition_sdk` target 不导出。

Nested consumer project 暴露 generator-aware 的 `run_photospider_consumer` target，
其 command 使用 executable 的 target-file expression。Outer gate 会传递精确 generator、
存在时的 platform/toolset 与 active configuration，随后 build 该 run target。因此
single-config 与 multi-config layout 都不需要猜测 build root、configuration directory、
executable suffix 或 bundle path。Outer gate 还会传递闭合的 producer sanitizer mode：
`none`、`address` 或 `thread`。Sanitized nested project 会给 C SDK object、shared
bridge 与 final executable 应用 matching compile instrumentation，并给两个 linked
product 应用 matching link instrumentation。因此，即使 static kernel 先嵌入一个 private
downstream shared bridge，final executable 仍直接拥有 sanitizer runtime。普通 consumer
不接收 sanitizer option，installed `PhotospiderTargets.cmake` 也绝不包含 sanitizer flag。

Daemon validation 必须使用该隔离 prefix，绝不能使用 sibling checkout 或 private
include directory。

Deterministic scheduler test 使用只编入 noninstalled `photospider_test_kernel` 的 private
callback-enqueue、pre-failure、queue-rejection 与 diagnostic-construction hook。它们暴露
otherwise unobservable 的 no-GPU/admission/submit linearization window 与 allocation-free
exception fallback。Construction hook 会精确选择 exception-fence 或 run-loop
external-stop materialization point，并在 owned diagnostic 与 `Status` construction 前
立即触发。External-stop fallback 在已经持有的 Run mutex 下运行，不减少 in-flight
slot；exception helper 则继续拥有其 callback-retirement 责任。Callback-enqueue observer
还会在唯一 CPU worker 被占用期间记录 target 与 successor 的 admission；等待 successor
Run 完成，即可在 held GPU callback 释放前证明 FIFO retirement。Regression 还让 null
standard-exception diagnostic 经过只接收 pointer 的调用边界；正常完成证明 failure 被
fence、in-flight count 完成 drain，且 empty-message fallback 仍可用。`BUILD_TESTING=ON`
时，product archive、installed kernel、export 与普通 consumer 仍不含 hook；
`BUILD_TESTING=OFF` 时，test-kernel target 与其 execution-hook object 都不存在。

同一个不安装的 execution-hook object 还在 complete local result assembly 后、final stop
check 前暴露一个 no-throw `final_result_ready` observer；它还在 coordinator 重新取得 Run
mutex 并检查 external stop 后、可能进入 wait 前暴露一个 no-throw post-submit observer。
前者只用于阻塞 success-publication linearization window，后者在不增加 production
notification path 的情况下证明 queued worker-entry admission。Product archive/package
不包含任一 observer symbol 或 test string。Native-library path regression 同样只通过不安装
的 test-kernel hook object 统计 loader、owner 与 close boundary。

## Sanitizer 与 malformed-input validation

当 C++ compiler 与 linker 支持请求的 instrumentation 时，ASAN 与 TSAN 是互斥的
scoped CMake mode；缺少支持时 configure 会失败，不会静默生成未插桩 target。Sanitizer
compile/link option 对 kernel product 为 private，仅通过其 build-tree interface 发布，
从而让每个 in-tree executable 对 runtime 闭合。完整 sanitizer CTest inventory 继续保留
installed-consumer gate；该 gate 使用显式 matching nested mode 验证 installed static/
shared-bridge/final-executable 拓扑，同时不向 installed package export 泄漏 instrumentation。
普通 test 覆盖 malformed Value/Region/layout、graph document、operation/provider record、
精确 library path 与 callback output。Malformed local IPC frame 属于 daemon repository。

长期手动 target `photospider_operation_contract_ir_fuzz` 覆盖 当前 OperationTraits 的
trait/parameter vocabulary 与 compiler validation。它使用 `EXCLUDE_FROM_ALL`，绝不注册到
CTest；Clang 下通过 `-DPHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS=ON` 显式启用。Seed input
维护在 `tests/fuzz/corpus/operation_contract_ir/`；调用者选择的 crash/artifact directory
保持 untracked。Bounded smoke run 为：

```bash
cmake -S . -B <fuzz-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DPHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS=ON -DBUILD_TESTING=OFF
cmake --build <fuzz-build> --target photospider_operation_contract_ir_fuzz -j
ps_operation_fuzz_corpus=$(mktemp -d)
cp -R tests/fuzz/corpus/operation_contract_ir/. \
  "$ps_operation_fuzz_corpus"/
<fuzz-build>/photospider_operation_contract_ir_fuzz \
  "$ps_operation_fuzz_corpus" -runs=1000 -max_len=256
```

固定负向 DSO fixture 继续作为 raw ABI pointer/size/alignment/count/bounds case 的权威
测试，因为 byte-only in-process harness 无法安全构造这些 case。
使用确实提供 libFuzzer runtime 的 Clang distribution；只报告 Clang identity 但缺少该
archive 仍不满足条件。Temporary working corpus 防止 generated mutation 进入 maintained
seed directory。

普通 CTest `test_operation_contract_ir_seeds` 只读两个 committed seed，不生成 mutation。
它证明 `valid-source` 到达并通过 compiler path，同时 `malformed-schema` 构造 duplicate
parameter schema 并到达命名的 registry rejection。该 deterministic stage seam 补充但不
注册 manual libFuzzer target。

## CTest 所有权

CTest 验证可观察正确性，包括数值结果、资源边界、并发、错误处理、安装消费、
编译及运行边界。性能测量和依赖计时的诊断保持为可选工具，不以耗时阈值定义正确性。不得注册 stale-term
search、source-layout audit、process checklist、Doxygen audit、Issue replay 或
result/provenance orchestration。Manual source-quality tool 需要维护的中英文文档，并
保持在 CTest/CI 之外。使用 Clang/GCC 对 source-tree 与 installed-tree header 做 direct
self-containment scan 属于这种 manual check；不得注册到 CTest 或 CI。

## Final command

`kernel-dev` preset 是本地 clean validation 的默认入口。删除 `build/` 会清除该目录下
所有 ignored build 配置；保留源码文件及其外部的其他 ignored 数据。随后配置并构建
preset，再运行完整 CTest 清单。该 preset 使用本机 Clang、RelWithDebInfo、
`BUILD_TESTING=ON` 和平台默认的可选 backend。CTest JUnit 报告路径由
`--output-junit` 显式指定；相对路径以 preset 的 binary directory
`build/kernel-dev` 为基准。

```bash
rm -rf build
cmake --preset kernel-dev
cmake --build --preset kernel-dev -j
ctest --preset kernel-dev --output-on-failure --output-junit ctest-results.xml
```

完整 CTest 包含 `test_installed_consumer`，会安装到 fresh prefix，并配置、构建和运行
外部 consumer。只有需要额外配置时才单独运行 installed gate。

使用 ClangFormat 21 格式化 changed C/C++，并对相同文件运行
`python3 -m cpplint`。不支持的 sanitizer/GPU platform 记录为 limitation，而不是
successful gate。

## Focused Result validation

最终 focused core run 的以下十三项测试全部通过：`test_dependency_dirty`、
`test_result_execution`、`test_result_plugin`、`test_result_native_gpu`、
`test_result_image_contracts`、`test_unified_result_images`、
`test_global_results`、`test_shared_results`、`test_result_metadata_budget`、
`test_resources`、`test_execution_dependencies`、`test_multi_output_execution` 和
`test_generic_result_cache`。这些测试覆盖 C Result 混合 outputs、图像 schema 推导、prefix/零行
fields、typed Field/Descriptor replacement、64 个 sparse fragments、具名 C++ outputs、
动态 Control-to-Data support、Unknown relation union 与 declared input projections、returned dependency map ownership、
multi-output execution、shared Result caching 和 dependency owner admission。C fixture 通过同步 sink 使用 callback-local 嵌套 records
解析 output metadata；宿主在 `set_output` 返回前复制这些 records。Image contract tests 验证
大 facet allocation 前的 typed image metadata admission，以及 C resolver 对非法零 slot/多 slot
schema 的拒绝行为。

`test_result_execution` 还使用 64-byte 和 256-byte windows 检查零行、部分行与 8192 行 discovery。
Resource tests 覆盖 retained Result owners 和 metadata 限额。Focused suite 用时约三秒。

Native GPU tests 通过独立的 native GPU service ABI 与 Result operation callbacks 执行，覆盖 dispatch、readback、sticky service errors 和有界 tensor-window transfer。没有兼容硬件时 native CTest 返回 77；硬件执行结果必须与 CPU fallback 分开报告。

复跑 core set 时，构建这些 test executables 并运行相应 CTest：

```sh
cmake --build build/kernel-dev --target test_dependency_dirty test_result_execution test_result_plugin test_unified_result_images test_global_results test_shared_results test_result_metadata_budget test_resources test_result_image_contracts test_execution_dependencies test_multi_output_execution test_generic_result_cache -j8
ctest --test-dir build/kernel-dev -R '^test_(dependency_dirty|result_execution|result_plugin|result_native_gpu|result_image_contracts|unified_result_images|global_results|shared_results|result_metadata_budget|resources|execution_dependencies|multi_output_execution|generic_result_cache)$' --output-on-failure
```

source-support admission fence、numeric descriptor guard 和 affine/broadcast Result 修改之后，最终 installed package run 的以下五项测试全部通过：
`installed_unified_result_workflow`、`installed_unified_result_cpp`、
`installed_unified_result_contracts`、`installed_unified_result_c11` 和
`installed_unified_result_native_gpu`。Native case 使用 Metal，未 skip。日志位于
`build/kernel-dev/unified-consumer.log`。

```sh
cmake --install build/kernel-dev --prefix build/unified-result-install
cmake -S tests/consumer -B build/unified-result-consumer -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/Users/zhufeng/document/code/photospider/build/unified-result-install
cmake --build build/unified-result-consumer --target run_unified_result_consumer -j8
```

Installed tests 运行 public C++ workflow、C++ workflow consumer、typed image-contract consumer、
C11 Result DSO consumer 和 native GPU Result DSO consumer。Installed contract consumer 还覆盖 typed source-support budget refusal。Native test 在 Metal 上覆盖当前 affine 和 broadcast transfer 行为。这些 focused checks 不代表完整 CTest、sanitizer 或 release 平台矩阵验证。

## 安装与可选覆盖边界

`test_installed_consumer` 配置隔离 prefix、安装 package，再通过 `find_package(Photospider CONFIG REQUIRED)` 构建并运行 external consumer。`tests/consumer/CMakeLists.txt` 中的实际 runtime command list 是判断依据。只出现在 `DEPENDS` 中的 target 属于 build coverage，不属于 runtime coverage。当前 `run_unified_result_consumer` target 会执行 public minimal workflow、C++ workflow consumer、typed image-contract consumer、C11 DSO consumer 和 native GPU consumer。

Result image 行为由上文列出的 Result fixtures 和 installed consumers 覆盖。Production image 可用性按当前 catalog 与 runtime 契约执行，详见[图像 operations](../../kernel-architecture/zh/Image-Operations.zh.md)。

使用自定义 compiler/runtime 时，将相同的 `CC` 与 `CXX` 环境传给 CTest，因为 installed gate 会配置 nested consumer。Unsupported GPU 或 sanitizer 能力记录为限制，不记录为测试通过。
