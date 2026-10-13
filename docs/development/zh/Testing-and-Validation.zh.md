# 测试与验证

本文说明已注册的行为测试。`CMakeLists.txt` 与 `cmake/BehaviorTests.cmake` 中的声明共同定义每个测试的可执行文件、fixture、依赖、命令、标签、timeout 和 skip code。

## 配置与运行

配置前先将固定版本 SLEEF 3.9.0 源码放到 `third_party/sleef/`；CMake 不会自动下载。常规本地构建命令如下：

```sh
cmake -S . -B build/check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/check -j8
ctest --test-dir build/check --output-on-failure
```

`photospider_tests` 构建全部已注册测试可执行文件。CTest 标签可以选择行为族，例如 `ctest --test-dir build/check -L 'unit|execution' --output-on-failure`；`ctest --test-dir build/check --print-labels` 可查看标签。Unit 测试 timeout 为 120 秒，integration 为 300 秒，大型 numeric 或 image 测试为 600 秒。Sanitizer build 将各自 timeout 延长三倍。

需要 private symbols 的测试链接非安装的 static test library，该库使用正常 kernel objects 构建；fault-injection 变体只替换包含对应 test seam 的编译单元。该库同时编译私有 execution test hooks，并发测试借此等待真实事件，不依赖计时。Phase work gate 让 callback 线程停在 executor poll 内选定的一次正值 work 计费处。Operation fixture DSO 导出 operation ABI 之外的私有 arm、wait 和 release 符号，用于挂起 native callback 直到测试放行；放行后 callback 调用真实的 cancellation service。

仓内的 `MODULE` fixture 使用相同的 sanitizer instrumentation；C11 operation fixture 仍以 C 编译。macOS ASAN 构建会在每个 module 中加入测试专用 C++ atexit owner，在 `dlclose` 执行模块退休期间、image 仍映射时注销 compiler-rt image globals。实现见[module instrumentation helper](../../../cmake/BehaviorTests.cmake)和[retirement owner](../../../tests/support/asan_module_retirement.cpp)。

## C Result table 拒绝测试

共享的[`photospider_add_result_table_fixture` helper](../../../cmake/BehaviorTests.cmake)将坏 Result table fixture 编译为 C11 `MODULE` 库。两个带 selector 的 DSO 覆盖 20 个非法表案例：operation 14 个、repeated input 6 个。两个基础 fixture 分别是[`result_operation_fixture.c`](../../../tests/fixtures/result_operation_fixture.c)、[`result_repeated_fixture.c`](../../../tests/fixtures/result_repeated_fixture.c)，共用[bad-table selector support](../../../tests/fixtures/bad_result_table_support.h)。[测试 helper](../../../tests/support/bad_result_table_fixture.hpp)选择一个案例并调用真实的 `OperationRegistry::load_plugin`；每个案例检查预期的 `Status.code`、空 registry key 列表和准确的 plugin destroy 数量。关闭 observer handle 前先销毁 registry 和 candidate，以便 DSO 仍映射时验证退休行为。

Root integration 测试为[`test_result_plugin`](../../../tests/integration/test_result_plugin.cpp)、[`test_result_repeated`](../../../tests/integration/test_result_repeated.cpp)。Fixture 构建依赖由[root CMake 注册声明](../../../CMakeLists.txt)提供。聚焦运行命令如下：

```sh
cmake --build build/check --target \
  test_result_plugin test_result_repeated -j8
ctest --test-dir build/check \
  -R '^(test_result_plugin|test_result_repeated)$' \
  --output-on-failure
```

## 行为边界

已注册测试覆盖 compiler 校验与规划、Result operations 和 C ABI 加载、执行与取消、依赖关系、资源限制、数值与图像行为以及原生 backend dispatch。

硬件专属测试以 skip return code 77 注册。确定性 CPU 或 fault-injection 测试可以覆盖相关软件契约，但不能证明原生设备执行。报告时应分别说明 CPU/mock 覆盖和实际硬件执行。

测试检查可观察的数值、错误类别、资源限制、取消、发布和所有权行为。Block-cache 命中数等缓存内部计数属于实现细节；性能基准与行为测试分开处理。
