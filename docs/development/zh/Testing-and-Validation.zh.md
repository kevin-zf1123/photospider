# 测试与验证

本文说明已注册的行为测试。`cmake/KernelTests.cmake`、`cmake/PackageTests.cmake` 与 `cmake/BehaviorTests.cmake` 中的声明共同定义每个测试的可执行文件、fixture、依赖、命令、标签、timeout 和 skip code。

## 测试归属与保留周期

常驻测试覆盖内核行为和包契约。内核单元测试放在 `tests/unit/kernel/`，集成测试
放在 `tests/integration/kernel/`。它们使用合成 operation 和 fixture 检查编译器、
执行、ABI、资源及所有权行为，并在 `PHOTOSPIDER_BUILD_OPS=OFF` 时构建。
包契约测试放在 `tests/package/`；版本检查直接求值生成的 SameMinorVersion 配置，
不安装包，也不编译外部 consumer。

内置算子的数值测试、算子执行集成测试、跨算子 workflow、示例和 benchmark 不作为
常驻测试集保留。实现或修改算子时，针对本次改动现场编写必要的公开
执行路径检查。临时测试源码及生成数据放在忽略的 build/output 目录；验证和算子修改完成后，
立即删除检查程序、数据和临时构建／测试注册。数学定义、容差与行为验收要求保留在算子规格中。

内核 fixture 可以使用简单算术来观察调度或 Result 行为；这不构成内置算子的数值验收。
测试必须检查程序行为，迁移状态、已删除文件清单、流程文字和源码质量评分不进入测试。

独立算子 oracle 及其参考数据长期保留，用于后续算子实现，包括 Proposed 规格。
上述临时测试保留周期适用于实现的测试。Oracle 入口见
[`oracle/ops/`](../../../oracle/ops/README.md)。

## 配置与运行

常规本地构建命令如下：

```sh
cmake -S . -B build/check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DPHOTOSPIDER_BUILD_OPS=OFF
cmake --build build/check --target photospider_kernel_tests -j8
ctest --test-dir build/check -L '^(kernel|package)$' --output-on-failure
```

`photospider_tests` 构建全部已注册测试可执行文件。CTest 标签可以选择行为族，例如 `ctest --test-dir build/check -L 'kernel|package' --output-on-failure`；`ctest --test-dir build/check --print-labels` 可查看标签。归属标签为 `kernel`、`package`，层级标签为 `unit`、`integration`、`contract`。
GPU、mock、native、backend 与 ABI 标签在注册处显式指定，不按测试名猜测。
Unit 测试 timeout 为 120 秒，integration 为 600 秒，包契约为 60 秒。Sanitizer build 将各自 timeout 延长三倍。

需要 private symbols 的测试链接非安装的 static test library，该库使用正常 kernel objects 构建；fault-injection 变体只替换包含对应 test seam 的编译单元。该库同时编译私有 execution test hooks，并发测试借此等待真实事件，不依赖计时。Phase work gate 让 callback 线程停在 executor poll 内选定的一次正值 work 计费处。Operation fixture DSO 导出 operation ABI 之外的私有 arm、wait 和 release 符号，用于挂起 native callback 直到测试放行；放行后 callback 调用真实的 cancellation service。

仓内的 `MODULE` fixture 使用相同的 sanitizer instrumentation；C11 operation fixture 仍以 C 编译。macOS ASAN 构建会在每个 module 中加入测试专用 C++ atexit owner，在 `dlclose` 执行模块退休期间、image 仍映射时注销 compiler-rt image globals。实现见[module instrumentation helper](../../../cmake/BehaviorTests.cmake)和[retirement owner](../../../tests/support/asan_module_retirement.cpp)。

## C Result table 拒绝测试

共享的[`photospider_add_result_table_fixture` helper](../../../cmake/BehaviorTests.cmake)将坏 Result table fixture 编译为 C11 `MODULE` 库。两个带 selector 的 DSO 覆盖 20 个非法表案例：operation 14 个、repeated input 6 个。两个基础 fixture 分别是[`result_operation_fixture.c`](../../../tests/fixtures/result_operation_fixture.c)、[`result_repeated_fixture.c`](../../../tests/fixtures/result_repeated_fixture.c)，共用[bad-table selector support](../../../tests/fixtures/bad_result_table_support.h)。[测试 helper](../../../tests/support/bad_result_table_fixture.hpp)选择一个案例并调用真实的 `OperationRegistry::load_plugin`；每个案例检查预期的 `Status.code`、空 registry key 列表和准确的 plugin destroy 数量。关闭 observer handle 前先销毁 registry 和 candidate，以便 DSO 仍映射时验证退休行为。

Root integration 测试为[`test_result_plugin`](../../../tests/integration/kernel/test_result_plugin.cpp)、[`test_result_repeated`](../../../tests/integration/kernel/test_result_repeated.cpp)。Fixture 构建依赖由[root CMake 注册声明](../../../CMakeLists.txt)提供。聚焦运行命令如下：

```sh
cmake --build build/check --target \
  test_result_plugin test_result_repeated -j8
ctest --test-dir build/check \
  -R '^(test_result_plugin|test_result_repeated)$' \
  --output-on-failure
```

## 行为边界

已注册测试覆盖 compiler 校验与规划、Result operations 和 C ABI 加载、执行与取消、依赖关系、资源限制以及原生 backend dispatch。

Mock/service 检查与原生 GPU 检查分别注册；只有依赖硬件的注册项使用 skip code 77。
原生检查跳过时，mock/service 的通过结果仍独立记录。确定性的 CPU 或故障注入检查
不构成原生设备执行证据，应分别报告两类覆盖。

`test_gpu_c_abi_bridge` 以 `test_gpu_c_abi_execution --bridge-only` 运行，标签为
`kernel`、`integration`、`mock`、`abi`、`gpu`。它不需要 GPU，也没有 skip code。
它检查非法 writable 标志、伪造或过期 token 在调用 host callback 前被拒绝，并用
合法 token 翻译作为正向对照。独立的 `test_gpu_c_abi_execution` 注册项覆盖原生执行。
软件 bridge 检查可单独运行：

```sh
cmake --build build/check --target test_gpu_c_abi_execution -j8
ctest --test-dir build/check -R '^test_gpu_c_abi_bridge$' -LE native --output-on-failure
```

`tests/integration/kernel/gpu/` 中的 MSL 集成测试在其他构建中仍运行前面的 CPU 检查，
随后仅在编译了 Metal（`PHOTOSPIDER_ENABLE_METAL` 与 `APPLE` 均成立）且 context 有
可用 GPU 时进入原生阶段。Metal 单元测试检查实际设备后端。原生阶段接纳后出现的
非预期 backend 错误仍算测试失败，不转换成跳过。

测试检查可观察的数值、错误类别、资源限制、取消、发布和所有权行为。Block-cache 命中数等缓存内部计数属于实现细节；性能基准与行为测试分开处理。

负向案例只设置一个非法条件，其余输入保持合法，防止另一项拒绝满足相同的结构化
错误断言。错误传播测试比较捕获的原始 `Status`，包括 message，避免固定生成的
英文文案。契约规定的收费和直接生命周期事件仍适合检查精确计数；契约仅规定上界时，
检查该上界并显式触发拒绝。通过实际计算和所有权观察验证复用及退休行为。
