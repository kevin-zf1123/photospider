# GitHub Actions CI

Kernel CI 验证有用 build、test、runtime 与 installed-package signal；不实现企业审批、
migration-provenance 或 evidence aggregation。

## Kernel workflow

维护的 kernel workflow 执行：

1. 在当前 Ubuntu/macOS runner 上执行 CMake/Ninja configure；
2. full build dependency-neutral kernel 与维护的 test；
3. complete CTest，其中包含 isolated install/package consumer；
4. 独立 Linux ASAN 与 TSAN configure/build/test job。

可选 GPU compile/runtime 只在兼容 runner 运行。CPU test 保持必需，GPU 不可用 path
验证 planner fallback。ASAN/TSAN 可作为支持 toolchain 的独立 job，不改变产品 API。

## 跨仓边界

Kernel CI 不 checkout daemon。Daemon CI 在存在同名 kernel feature branch 时选择它，
否则选择 kernel main；随后 build/install 到 isolated prefix，并且只针对该 public
package 配置。Private header、copied IR 或 daemon-to-kernel source-tree include 都是
failure。

Daemon CI 验证 local IPC v3、Session/Job lifecycle、cancellation、restart loss、
result release、shutdown、package consumer、public dependency inventory 与 malformed
frame。它不含 IPC v2 four-cell compatibility gate。

## Test 所有权

CTest/CI 只运行长期产品行为。Migration residue、stale-term search、source-layout
completion、Doxygen audit、Issue replay、result orchestration 与 provenance report
仍是 manual development check，不注册。

## Test output

普通 job 输出 CTest JUnit 与 runner log。这些文件只是 CI diagnostic，不会成为
runtime product object 或 release authority。

## 本地一致性

Developer 运行 native focused check 与
[Testing and Validation](../../development/zh/Testing-and-Validation.zh.md) 描述的一次
final native clean pass。本地不需要 Docker 或 architecture emulation 来模拟 hosted
CI。
