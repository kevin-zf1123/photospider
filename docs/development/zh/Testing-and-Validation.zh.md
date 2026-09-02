# 测试与验证

本文定义 breaking scope reset 后维护中的 repository validation。Test 验证长期软件
行为，不验证迁移完成或 provenance。

## 开发循环

实现期间使用 scoped formatting/lint、affected target 与 focused test。Source 与
documentation 冻结后，最多运行一次 native clean configure、一次 full build 与一次
完整 CTest/JUnit。该 final pass 不使用 Docker 或本地 architecture emulation。

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
  hook 后 GPU execution 恢复成功。另有独立 CPU queue-rejection case 保留 CPU 覆盖；
- bounded ready work 与 `ResourceLedger` settlement；
- cross-backend copy/backend label、cancellation、stale completion 与 exception fence；
- Value/Region/strided-layout/facet/buffer 负向契约；
- operation/provider ABI version/size/alignment/pointer/count/bounds/lifetime，包括
  operation-v2 typed parameter schema、demand view，以及带精确 destroy/close count 的
  deterministic owner-allocation failure。若 C++ embedding callback 抛出 `what()` 为
  null 的标准异常，则它会被隔离为带空 message 的 `OperationFailed`；
  `std::bad_alloc` 仍可由 embedding caller 观察；
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
  failure 保留为 sample，后续 iteration 继续。由于 monotonic clock 可能只有微秒
  分辨率，duration 允许为零。Deterministic regression 覆盖另一线程在 oracle barrier
  中取消、self-cancelling true/false/throwing oracle，以及通过 noninstalled test-kernel
  seam 暴露的无 oracle post-execute window。

Daemon test 位于 `photospider-daemon`，覆盖 local frame validation、九方法 routing、
临时 Session/Job lifecycle、restart loss、multi-Session behavior、cancellation、
Session close、result release、shutdown 与隔离 installed-kernel boundary。

## Installed boundary

Package gate 将 Photospider 配置并安装到 fresh prefix，再只通过
`find_package(Photospider CONFIG REQUIRED)` 配置 external C/C++17 consumer。CI 会为
默认 static kernel 与 `BUILD_SHARED_LIBS=ON` 都运行该 gate。它验证：

- installed header 与声明 public inventory 完全一致；
- export 不含 source/private path；
- linked C SDK compilation unit 会实际运行；downstream shared bridge 会链接
  `Photospider::kernel`、执行 C++ compile/execute pipeline，并由 consumer executable
  调用。因此默认 static archive 会作为 position-independent input 被真实 shared
  library 使用；
- `kernel`、`operation_sdk` 与 `data_provider_sdk` component discovery 精确导出
  `Photospider::kernel`、`Photospider::operation_sdk` 与
  `Photospider::data_provider_sdk`；两个 SDK target 都只包含 header；
- 被删 target、header、component 与 executable 缺失。

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
exception fallback。Construction hook 在 no-throw helper 内部、owned diagnostic 与
`Status` materialization 之前立即触发。Regression 还让 null standard-exception
diagnostic 经过只接收 pointer 的调用边界；正常完成证明 failure 被 fence、in-flight
count 完成 drain，且 empty-message fallback 仍可用。`BUILD_TESTING=ON` 时，product
archive、installed kernel、export 与普通 consumer 仍不含 hook；`BUILD_TESTING=OFF`
时，test-kernel target 与其 execution-hook object 都不存在。

## Sanitizer 与 malformed-input validation

当 C++ compiler 与 linker 支持请求的 instrumentation 时，ASAN 与 TSAN 是互斥的
scoped CMake mode；缺少支持时 configure 会失败，不会静默生成未插桩 target。Sanitizer
compile/link option 对 kernel product 为 private，仅通过其 build-tree interface 发布，
从而让每个 in-tree executable 对 runtime 闭合。完整 sanitizer CTest inventory 继续保留
installed-consumer gate；该 gate 使用显式 matching nested mode 验证 installed static/
shared-bridge/final-executable 拓扑，同时不向 installed package export 泄漏 instrumentation。
普通 test 覆盖 malformed Value/Region/layout、graph document、operation/provider record
与 callback output。Malformed local IPC frame 属于 daemon repository。

长期手动 target `photospider_operation_contract_ir_fuzz` 覆盖 operation-v2
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

CTest/CI entry 只用于 correctness、performance、stability、multithreading、error
handling、package consumption、compilation 与 runtime boundary。不得注册 stale-term
search、source-layout audit、migration checklist、Doxygen audit、Issue replay 或
result/provenance orchestration。Manual source-quality tool 需要维护的中英文文档，并
保持在 CTest/CI 之外。

## Final command

精确 final build directory 与可选 capability flag 记录在 completion report。通常形态：

```bash
cmake -S . -B <clean-build> -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON
cmake --build <clean-build> -j
ctest --test-dir <clean-build> --output-on-failure --output-junit <report>
cmake --install <clean-build> --prefix <fresh-prefix>
```

使用 ClangFormat 21 格式化 changed C/C++，并对相同文件运行
`python3 -m cpplint`。不支持的 sanitizer/GPU platform 记录为 limitation，而不是
successful gate。
