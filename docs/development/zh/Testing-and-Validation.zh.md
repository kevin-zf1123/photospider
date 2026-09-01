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
- bounded ready work 与 `ResourceLedger` settlement；
- cross-backend copy/backend label、cancellation、stale completion 与 exception fence；
- Value/Region/strided-layout/facet/buffer 负向契约；
- operation/provider ABI version/size/alignment/pointer/count/bounds/lifetime；
- 不生成 verdict/evidence output 的 raw benchmark diagnostic。

Daemon test 位于 `photospider-daemon`，覆盖 local frame validation、九方法 routing、
临时 Session/Job lifecycle、restart loss、multi-Session behavior、cancellation、
Session close、result release、shutdown 与隔离 installed-kernel boundary。

## Installed boundary

Package gate 将 Photospider 配置并安装到 fresh prefix，再只通过
`find_package(Photospider CONFIG REQUIRED)` 配置 external C++17 consumer。它验证：

- installed header 与声明 public inventory 完全一致；
- export 不含 source/private path；
- embedded compile/execute facade 可链接并运行；
- `kernel`、`operation_sdk` 与 `data_provider_sdk` component discovery 精确导出
  `Photospider::kernel`、`Photospider::operation_sdk` 与
  `Photospider::data_provider_sdk`；两个 SDK target 都只包含 header；
- 被删 target、header、component 与 executable 缺失。

Daemon validation 必须使用该隔离 prefix，绝不能使用 sibling checkout 或 private
include directory。

## Sanitizer 与 malformed-input validation

Toolchain 支持时，ASAN 与 TSAN 是 scoped CMake mode。普通 test 覆盖 malformed
Value/Region/layout、graph document、operation/provider record 与 callback output。
Malformed local IPC frame 属于 daemon repository。当前没有维护 fuzz executable；
新增 fuzz target 必须具备长期 correctness 价值与 corpus policy，不能是 migration wiring。

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
