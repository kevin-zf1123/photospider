#!/usr/bin/env python3
"""Install Photospider and verify an external CMake consumer can link it."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from textwrap import dedent
from typing import Any, Callable

from cmake_build_smoke_support import (
    producer_osx_architecture_arguments,
)


STATIC_PRODUCT_ARCHIVE_NAMES = {
    "libphotospider.a",
    "libphotospider.lib",
    "photospider.lib",
}
INTERNAL_TEST_PRODUCT_STEM = "photospider_internal_test_product"
INTERNAL_PRODUCT_TEST_DEFINITIONS = (
    "PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING",
    "PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING",
    "PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING",
    "PHOTOSPIDER_INTERNAL_KERNEL_CLOSE_TESTING",
    "PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING",
)
FORBIDDEN_PRODUCT_TEST_SYMBOL_FRAGMENTS = (
    "reserved_start_probe_state",
    "record_reserved_start_attempt_for_testing",
    "arm_reserved_start_rollback_probe_for_testing",
    "reserved_start_rollback_probe_snapshot_for_testing",
    "disarm_reserved_start_rollback_probe_for_testing",
    "g_graph_cache_service_test_hook",
    "set_graph_cache_service_test_hook",
    "notify_graph_cache_service_test_hook",
    "g_graph_state_executor_test_hook",
    "set_graph_state_executor_test_hook",
    "notify_graph_state_executor_test_hook",
    "g_close_publish_test_hook",
    "set_graph_state_executor_close_publish_test_hook",
    "notify_graph_state_executor_close_publish_test_hook",
    "publish_graph_state_executor_test_snapshot",
    "g_kernel_close_test_hook",
    "set_kernel_close_test_hook",
    "notify_kernel_close_test_hook",
    "g_kernel_compute_commit_test_hook",
    "set_kernel_compute_commit_test_hook",
    "notify_kernel_compute_commit_test_hook",
)
REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS = (
    "ExecutionService17available_devices",
    "GraphCacheService17clear_drive_cache",
    "GraphStateExecutor15close_and_drain",
    "Kernel11close_graph",
    "Kernel30execute_staged_compute_request",
    "Kernel8shutdown",
)
EXPORTED_TARGET = "Photospider::photospider"
IPC_EXPORTED_TARGET = "Photospider::photospider_ipc_client"
REQUIRED_IPC_PUBLIC_HEADERS = {
    "include/photospider/ipc/client.hpp",
    "include/photospider/ipc/host.hpp",
    "include/photospider/ipc/protocol.hpp",
}
ALLOWED_IPC_STANDARD_INCLUDES = {
    "cstddef",
    "cstdint",
    "map",
    "memory",
    "optional",
    "string",
    "utility",
    "vector",
}
EXPECTED_IPC_DIRECT_INCLUDES = {
    "include/photospider/ipc/client.hpp": (
        "cstddef",
        "cstdint",
        "map",
        "memory",
        "optional",
        "string",
        "vector",
        "photospider/core/export.hpp",
        "photospider/host/graph_session.hpp",
        "photospider/host/host.hpp",
        "photospider/ipc/protocol.hpp",
    ),
    "include/photospider/ipc/host.hpp": (
        "memory",
        "string",
        "photospider/core/export.hpp",
        "photospider/host/host.hpp",
    ),
    "include/photospider/ipc/protocol.hpp": (
        "cstddef",
        "cstdint",
        "optional",
        "string",
        "utility",
        "vector",
        "photospider/core/image_buffer.hpp",
        "photospider/core/result_types.hpp",
        "photospider/host/compute_request.hpp",
    ),
}
FORBIDDEN_IPC_DECLARATION_PATTERNS = {
    "backend implementation type": (
        r"\b(?:Kernel|GraphModel|InteractionService|ComputeService|"
        r"PluginManager|PolicyRegistry)\b"
    ),
    "nlohmann JSON type or header": r"nlohmann(?:::|/)",
    "raw Unix socket type": r"\b(?:sockaddr|sockaddr_un|sa_family_t)\b",
    "raw descriptor declaration": r"\b(?:int|long)\s+(?:[A-Za-z_]\w*_)?fd\b",
    "raw file identity type": r"\b(?:dev_t|ino_t|mode_t|off_t)\b",
    "raw file mapping symbol": r"\b(?:mmap|munmap|MAP_FAILED|MAP_PRIVATE|PROT_READ)\b",
}
EXPECTED_CLIENT_METHODS = (
    "ping",
    "version",
    "load_graph",
    "close_graph",
    "list_graphs",
    "inspect_graph",
    "inspect_node",
    "inspect_dependency_tree",
    "reload_graph",
    "save_graph",
    "clear_graph",
    "get_node_yaml",
    "set_node_yaml",
    "list_node_ids",
    "ending_nodes",
    "traversal_orders",
    "traversal_details",
    "trees_containing_node",
    "project_roi",
    "project_roi_backward",
    "dirty_region_snapshot",
    "compute_planning_snapshot",
    "recent_compute_planning_snapshots",
    "submit_compute",
    "compute_status",
    "compute_result",
    "release_compute",
    "timing",
    "last_io_time",
    "last_error",
    "begin_dirty_source",
    "update_dirty_source",
    "end_dirty_source",
    "drain_compute_events",
    "clear_cache",
    "clear_drive_cache",
    "clear_memory_cache",
    "cache_all_nodes",
    "free_transient_memory",
    "synchronize_disk_cache",
    "plugins_load_report",
    "plugins_unload_all",
    "seed_builtin_ops",
    "ops_sources",
    "ops_combined_keys",
    "ops_combined_sources",
    "policy_available_types",
    "policy_description",
    "policy_scan",
    "policy_load",
    "policy_loaded_plugins",
    "configure_policy_defaults",
    "policy_info",
    "replace_policy",
    "execution_available_types",
    "execution_description",
    "configure_execution_defaults",
    "execution_info",
    "replace_execution",
    "execution_trace",
)
EXPECTED_HOST_METHODS = (
    "load_graph",
    "close_graph",
    "list_graphs",
    "reload_graph",
    "save_graph",
    "clear_graph",
    "compute",
    "compute_async",
    "compute_and_get_image",
    "timing",
    "last_io_time",
    "last_error",
    "list_node_ids",
    "ending_nodes",
    "get_node_yaml",
    "set_node_yaml",
    "inspect_node",
    "inspect_graph",
    "dependency_tree",
    "traversal_orders",
    "traversal_details",
    "trees_containing_node",
    "project_roi",
    "project_roi_backward",
    "dirty_region_snapshot",
    "compute_planning_snapshot",
    "recent_compute_planning_snapshots",
    "begin_dirty_source",
    "update_dirty_source",
    "end_dirty_source",
    "drain_compute_events",
    "execution_trace",
    "clear_cache",
    "clear_drive_cache",
    "clear_memory_cache",
    "cache_all_nodes",
    "free_transient_memory",
    "synchronize_disk_cache",
    "plugins_load_report",
    "plugins_load",
    "plugins_unload_all",
    "seed_builtin_ops",
    "ops_sources",
    "ops_combined_keys",
    "ops_combined_sources",
    "policy_available_types",
    "policy_description",
    "policy_scan",
    "policy_load",
    "policy_loaded_plugins",
    "configure_policy_defaults",
    "policy_info",
    "replace_policy",
    "execution_available_types",
    "execution_description",
    "configure_execution_defaults",
    "execution_info",
    "replace_execution",
)
FORBIDDEN_IPC_TARGET_PARTS = (
    "Photospider::photospider",
    "nlohmann_json",
    "photospider_ipc_client_objects",
    "photospider_ipc_server_internal",
)
REQUIRED_OPENCV_COMPONENTS = (
    "opencv_core",
    "opencv_imgproc",
    "opencv_imgcodecs",
    "opencv_videoio",
)
OPENCV_COMPONENT_TARGET_VARIANTS = {
    "opencv_core": ("opencv_core", "OpenCV::opencv_core", "OpenCV::Core"),
    "opencv_imgproc": (
        "opencv_imgproc",
        "OpenCV::opencv_imgproc",
        "OpenCV::Imgproc",
    ),
    "opencv_imgcodecs": (
        "opencv_imgcodecs",
        "OpenCV::opencv_imgcodecs",
        "OpenCV::Imgcodecs",
    ),
    "opencv_videoio": (
        "opencv_videoio",
        "OpenCV::opencv_videoio",
        "OpenCV::Videoio",
    ),
}
APPLE_PRODUCT_LINK_FLAGS = ("-framework Metal", "-framework Foundation")


@dataclass(frozen=True)
class SymbolToolCandidate:
    """@brief Name one validated archive-symbol inspection candidate.

    @param source Stable path-free label describing how the tool was found.
    @param executable Exact executable path passed to ``subprocess``.
    @throws None Construction only retains immutable strings.
    @note Diagnostics expose ``source`` rather than ``executable`` so a failed
      scan does not disclose a user or toolchain installation path.
    """

    source: str
    executable: str


@dataclass(frozen=True)
class SymbolToolFailure:
    """@brief Describe one path-free symbol-tool discovery failure.

    @param source Stable source label from the closed candidate inventory.
    @param reason Stable reason category containing no subprocess output/path.
    @param status Optional nonzero child status for discovery failures.
    @throws None Construction only retains immutable scalar values.
    @note The record never stores executable text, stdout, stderr, or PATH.
    """

    source: str
    reason: str
    status: int | None = None


@dataclass(frozen=True)
class SymbolToolResolution:
    """@brief Hold ordered unique symbol tools and discovery outcomes.

    @param candidates Validated candidates in their required platform order.
    @param failures Path-free discovery records for absent/unusable branches.
    @param sequence Candidate and failure records in source-priority order.
    @throws None Construction only retains immutable tuples.
    @note Duplicate canonical executable paths occur at most once in
      ``candidates`` and become path-free skipped records in ``sequence``.
    """

    candidates: tuple[SymbolToolCandidate, ...]
    failures: tuple[SymbolToolFailure, ...]
    sequence: tuple[SymbolToolCandidate | SymbolToolFailure, ...]


def strict_remove_tree(path: Path) -> None:
    """@brief Remove one transient test directory without hiding failure.

    @param path Directory whose previous contents must not survive this test.
    @return None.
    @throws OSError If filesystem inspection or recursive removal fails.
    @throws RuntimeError If removal returns while the path still exists.
    @note The helper never creates ``path``. Callers validate destructive paths
      before invoking it.
    """

    if path.exists() or path.is_symlink():
        shutil.rmtree(path)
    if path.exists() or path.is_symlink():
        raise RuntimeError(f"transient test directory still exists: {path}")


def run_command(
    command: list[str], cwd: Path, env: dict[str, str] | None = None
) -> int:
    """@brief Run one package-behavior command with CTest-visible output.

    @param command Executable and arguments passed without a shell.
    @param cwd Working directory for the child process.
    @param env Optional complete child environment; ``None`` inherits the caller.
    @return Child-process exit status without raising for nonzero results.
    @throws OSError If the command cannot start.
    @note Child standard output and error are inherited so CTest captures the
      original streams. The function changes no process-global environment and
      has no shell-expansion behavior.
    """

    print("$ " + shlex.join(command), flush=True)
    proc = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        env=env,
    )
    if proc.returncode != 0:
        print(
            f"command failed with exit code {proc.returncode}: " + shlex.join(command),
            file=sys.stderr,
            flush=True,
        )
    return proc.returncode


def run_captured_command(
    command: list[str], cwd: Path
) -> subprocess.CompletedProcess[str]:
    """@brief Run one diagnostic command with captured text streams.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Working directory for the child process.
    @return Completed process with replacement-decoded stdout and stderr.
    @throws OSError If the executable cannot start.
    @note This helper is reserved for short symbol-tool discovery/inspection;
      callers convert failures to path-free diagnostics and never echo captured
      stderr into the package verdict.
    """

    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )


def is_executable_file(path: str) -> bool:
    """@brief Validate one absolute regular executable path.

    @param path Candidate returned by an external discovery command.
    @return ``True`` only for an absolute regular file executable by this user.
    @throws None Filesystem lookup errors produce ``False`` through the
      ``Path.is_file`` and ``os.access`` contracts.
    @note PATH candidates already pass ``shutil.which``; this stricter helper
      validates untrusted text returned by ``xcrun --find``.
    """

    try:
        candidate = Path(path)
        return (
            candidate.is_absolute()
            and candidate.is_file()
            and os.access(candidate, os.X_OK)
        )
    except (OSError, ValueError):
        return False


def resolve_product_archive_symbol_tools(
    platform_system: str,
    cwd: Path,
    *,
    which: Callable[[str], str | None] = shutil.which,
    captured_runner: Callable[
        [list[str], Path], subprocess.CompletedProcess[str]
    ] = run_captured_command,
    executable_validator: Callable[[str], bool] = is_executable_file,
) -> SymbolToolResolution:
    """@brief Resolve ordered, unique archive-symbol inspection candidates.

    Darwin first invokes ``xcrun --find llvm-nm`` and accepts its result only
    when it is one absolute executable path. PATH ``llvm-nm`` and PATH ``nm``
    follow in that order. Other platforms never discover or invoke ``xcrun``
    and use only the two PATH candidates.

    @param platform_system Host platform name such as ``Darwin`` or ``Linux``.
    @param cwd Working directory for the optional ``xcrun`` discovery command.
    @param which Injectable executable lookup matching ``shutil.which``.
    @param captured_runner Injectable no-shell captured subprocess runner.
    @param executable_validator Injectable validator for ``xcrun`` output.
    @return Ordered candidates plus path-free discovery failures.
    @throws None ``OSError`` from ``xcrun`` startup is converted to a failure;
      injected callbacks are otherwise expected to honor their contracts.
    @note Canonical paths are de-duplicated without changing the first source's
      priority. No archive is read and no process-global environment changes.
    """

    sequence: list[SymbolToolCandidate | SymbolToolFailure] = []
    if platform_system == "Darwin":
        xcrun = which("xcrun")
        if xcrun is None:
            sequence.append(
                SymbolToolFailure(
                    source="xcrun llvm-nm",
                    reason="xcrun is unavailable",
                )
            )
        else:
            try:
                resolved = captured_runner(
                    [xcrun, "--find", "llvm-nm"], cwd
                )
            except OSError:
                sequence.append(
                    SymbolToolFailure(
                        source="xcrun llvm-nm",
                        reason="discovery could not start",
                    )
                )
            else:
                resolved_path = resolved.stdout.strip()
                if resolved.returncode != 0:
                    sequence.append(
                        SymbolToolFailure(
                            source="xcrun llvm-nm",
                            reason="discovery exited with nonzero status",
                            status=resolved.returncode,
                        )
                    )
                elif not resolved_path:
                    sequence.append(
                        SymbolToolFailure(
                            source="xcrun llvm-nm",
                            reason="discovery returned no path",
                        )
                    )
                elif "\n" in resolved_path or "\r" in resolved_path:
                    sequence.append(
                        SymbolToolFailure(
                            source="xcrun llvm-nm",
                            reason="discovery returned multiple lines",
                        )
                    )
                elif not Path(resolved_path).is_absolute():
                    sequence.append(
                        SymbolToolFailure(
                            source="xcrun llvm-nm",
                            reason="discovery returned a relative path",
                        )
                    )
                elif not executable_validator(resolved_path):
                    sequence.append(
                        SymbolToolFailure(
                            source="xcrun llvm-nm",
                            reason="discovery returned an unusable path",
                        )
                    )
                else:
                    candidate = SymbolToolCandidate(
                        source="xcrun llvm-nm",
                        executable=resolved_path,
                    )
                    sequence.append(candidate)

    for source, executable_name in (
        ("PATH llvm-nm", "llvm-nm"),
        ("PATH nm", "nm"),
    ):
        executable = which(executable_name)
        if executable is None:
            sequence.append(
                SymbolToolFailure(
                    source=source,
                    reason="executable is unavailable",
                )
            )
        else:
            candidate = SymbolToolCandidate(
                source=source, executable=executable
            )
            sequence.append(candidate)

    unique: list[SymbolToolCandidate] = []
    ordered: list[SymbolToolCandidate | SymbolToolFailure] = []
    canonical_paths: set[str] = set()
    for entry in sequence:
        if isinstance(entry, SymbolToolFailure):
            ordered.append(entry)
            continue
        canonical_path = os.path.normcase(os.path.realpath(entry.executable))
        if canonical_path in canonical_paths:
            ordered.append(
                SymbolToolFailure(
                    source=entry.source,
                    reason="duplicate executable was skipped",
                )
            )
            continue
        canonical_paths.add(canonical_path)
        unique.append(entry)
        ordered.append(entry)
    failures = tuple(
        entry for entry in ordered if isinstance(entry, SymbolToolFailure)
    )
    return SymbolToolResolution(tuple(unique), failures, tuple(ordered))


def run_installed_policy_contract_probe(plugin: Path) -> int:
    """@brief Invoke the fixture-only ABI probe from one built policy DSO.

    @param plugin Exact C11 or C++17 policy library selected from its manifest.
    @return Probe status, or 127 when loading or symbol resolution fails.
    @throws None Load and symbol failures are converted to status 127.
    @note The DSO contains no backend dependency. Its private probe exercises
      the same exact metadata/create/select/destroy table exported to the Host.
    """

    try:
        library = ctypes.CDLL(str(plugin))
        probe = library.ps_installed_policy_contract_probe
        probe.argtypes = []
        probe.restype = ctypes.c_int
        status = int(probe())
    except (AttributeError, OSError) as error:
        print(
            f"installed policy contract probe could not run: {error}",
            file=sys.stderr,
            flush=True,
        )
        return 127
    print(f"installed policy contract probe exited with {status}", flush=True)
    return status


def configure_fresh_producer(
    cmake_executable: str,
    repo: Path,
    build: Path,
    config: str,
    generator: str,
    build_testing: str,
) -> int:
    """@brief Configure a clean top-level Photospider producer tree.

    @param cmake_executable Exact CMake executable selected by the caller.
    @param repo Photospider source repository passed as the top-level source.
    @param build Dedicated producer build tree to delete and recreate.
    @param config Build configuration used as ``CMAKE_BUILD_TYPE`` when set.
    @param generator Optional CMake generator selected with ``-G``; an empty
      value preserves CMake's environment/default generator selection.
    @param build_testing ``ON`` or ``OFF`` for the producer's test targets.
    @return Configure process exit status.
    @throws ValueError If ``build`` is the source tree or one of its ancestors.
    @throws OSError If cleanup or command startup fails.
    @throws RuntimeError If cleanup returns while the build tree or cache remains.
    @note Cleanup is intentionally destructive only below the explicit
      ``--build`` path. The later product build still names the real
      ``photospider`` target, so disabling tests cannot bypass the product.
    """

    if build == repo or build in repo.parents:
        raise ValueError(
            f"refusing to remove source tree or ancestor as build path: {build}"
        )
    strict_remove_tree(build)
    command = [
        cmake_executable,
        "-S",
        str(repo),
        "-B",
        str(build),
        f"-DBUILD_TESTING={build_testing}",
    ]
    if generator:
        command.extend(["-G", generator])
    if config:
        command.append(f"-DCMAKE_BUILD_TYPE={config}")
    return run_command(command, repo)


def public_header_includes(repo: Path) -> list[str]:
    """@brief Generate includes for every installable public header.

    @param repo Repository root containing ``include/photospider``.
    @return Sorted ``#include`` directives relative to ``include``.
    @throws OSError If the public header tree cannot be traversed.
    @note The function is read-only and filters to C/C++ header suffixes used by
      the install rule.
    """

    public_root = repo / "include" / "photospider"
    headers = sorted(
        path.relative_to(repo / "include").as_posix()
        for path in public_root.rglob("*")
        if path.is_file() and path.suffix in {".h", ".hpp", ".hh", ".hxx"}
    )
    return [f"#include <{header}>" for header in headers]


def ipc_consumer_source() -> str:
    """@brief Build the complete installed IPC symbol-consumer source.

    @return C++17 source that executes safe lifecycle calls and references the
      complete typed Client and Host operation surfaces in the non-smoke branch.
    @throws None The source is one immutable in-memory string.
    @note The normal no-argument run requires no daemon. Passing an argument is
      intentionally outside the smoke contract and exists only to force link
      references for every current operation symbol.
    """

    return """#include <photospider/ipc/client.hpp>
#include <photospider/ipc/host.hpp>
#include <photospider/ipc/protocol.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

/**
 * @brief Detects a public ``cancel_compute`` compatibility member.
 * @tparam T Installed Host or Client contract under inspection.
 * @tparam Probe Substitution-only member-address expression.
 * @throws Nothing.
 * @note Issue #73 is private and must not add this public control shim.
 */
template <typename T, typename Probe = void>
struct HasCancelCompute : std::false_type {};

/** @copydoc HasCancelCompute */
template <typename T>
struct HasCancelCompute<T, std::void_t<decltype(&T::cancel_compute)>>
    : std::true_type {};

static_assert(!HasCancelCompute<ps::Host>::value,
              "installed Host must expose no cancellation shim");
static_assert(!HasCancelCompute<ps::ipc::Client>::value,
              "installed Client must expose no cancellation shim");
static_assert(static_cast<int>(ps::ipc::ComputeJobState::Queued) == 0 &&
                  static_cast<int>(ps::ipc::ComputeJobState::Running) == 1 &&
                  static_cast<int>(ps::ipc::ComputeJobState::Succeeded) == 2 &&
                  static_cast<int>(ps::ipc::ComputeJobState::Failed) == 3,
              "IPC v2 compute state inventory must remain unchanged");
static_assert(static_cast<int>(ps::OperationErrorDomain::None) == 0 &&
                  static_cast<int>(ps::OperationErrorDomain::Transport) == 1 &&
                  static_cast<int>(ps::OperationErrorDomain::Protocol) == 2 &&
                  static_cast<int>(ps::OperationErrorDomain::Graph) == 3 &&
                  static_cast<int>(ps::OperationErrorDomain::Daemon) == 4,
              "public status-domain inventory must remain unchanged");

/**
 * @brief References every typed Client call and non-destructor Host virtual.
 * @param client Disconnected direct client used only for link references.
 * @param host Complete IPC Host used only for virtual dispatch references.
 * @return Zero after all calls; the package smoke never executes this branch.
 * @throws Whatever the referenced operations throw if a caller deliberately
 *         executes the reference-only branch.
 * @note Keeping calls in one runtime branch forces the external executable to
 *       resolve all current installed IPC client and adapter symbols.
 */
int reference_complete_surface(ps::ipc::Client& client, ps::Host& host) {
  const ps::ipc::IpcSessionId ipc_session{};
  const ps::ipc::ComputeRequestId compute_id{};
  const ps::GraphSessionId host_session{};
  const ps::GraphLoadRequest load_request{};
  const ps::ipc::ComputeSubmitRequest submit_request{};
  const ps::HostComputeRequest compute_request{};
  const ps::PixelRect roi{};
  const ps::HostPolicyConfig policy_config{};
  const ps::HostExecutionConfig execution_config{};
  const std::vector<std::string> paths{};
  const std::string text;
  constexpr ps::NodeId node{0};
  constexpr ps::ComputeIntent intent =
      ps::ComputeIntent::GlobalHighPrecision;
  constexpr ps::DirtyDomain dirty_domain = ps::DirtyDomain::HighPrecision;

  (void)client.ping();
  (void)client.version();
  (void)client.load_graph(load_request);
  (void)client.close_graph(ipc_session);
  (void)client.list_graphs();
  (void)client.inspect_graph(ipc_session);
  (void)client.inspect_node(ipc_session, node);
  (void)client.inspect_dependency_tree(ipc_session, std::nullopt, false);
  (void)client.reload_graph(ipc_session, text);
  (void)client.save_graph(ipc_session, text);
  (void)client.clear_graph(ipc_session);
  (void)client.get_node_yaml(ipc_session, node);
  (void)client.set_node_yaml(ipc_session, node, text);
  (void)client.list_node_ids(ipc_session);
  (void)client.ending_nodes(ipc_session);
  (void)client.traversal_orders(ipc_session);
  (void)client.traversal_details(ipc_session);
  (void)client.trees_containing_node(ipc_session, node);
  (void)client.project_roi(ipc_session, node, roi, node);
  (void)client.project_roi_backward(ipc_session, node, roi, node);
  (void)client.dirty_region_snapshot(ipc_session);
  (void)client.compute_planning_snapshot(ipc_session);
  (void)client.recent_compute_planning_snapshots(ipc_session);
  (void)client.submit_compute(submit_request);
  (void)client.compute_status(compute_id);
  (void)client.compute_result(compute_id);
  (void)client.release_compute(compute_id);
  (void)client.timing(ipc_session);
  (void)client.last_io_time(ipc_session);
  (void)client.last_error(ipc_session);
  (void)client.begin_dirty_source(ipc_session, node, dirty_domain, roi);
  (void)client.update_dirty_source(ipc_session, node, dirty_domain, roi);
  (void)client.end_dirty_source(ipc_session, node, dirty_domain);
  (void)client.drain_compute_events(ipc_session, 1);
  (void)client.clear_cache(ipc_session);
  (void)client.clear_drive_cache(ipc_session);
  (void)client.clear_memory_cache(ipc_session);
  (void)client.cache_all_nodes(ipc_session, text);
  (void)client.free_transient_memory(ipc_session);
  (void)client.synchronize_disk_cache(ipc_session, text);
  (void)client.plugins_load_report(paths);
  (void)client.plugins_unload_all();
  (void)client.seed_builtin_ops();
  (void)client.ops_sources();
  (void)client.ops_combined_keys();
  (void)client.ops_combined_sources();
  (void)client.policy_available_types();
  (void)client.policy_description(text);
  (void)client.policy_scan(paths);
  (void)client.policy_load(text);
  (void)client.policy_loaded_plugins();
  (void)client.configure_policy_defaults(policy_config);
  (void)client.policy_info(ps::PolicyClass::Interactive);
  (void)client.replace_policy(ps::PolicyClass::Interactive, text);
  (void)client.execution_available_types();
  (void)client.execution_description(text);
  (void)client.configure_execution_defaults(execution_config);
  (void)client.execution_info(ipc_session, intent);
  (void)client.replace_execution(ipc_session, intent, text);
  (void)client.execution_trace(ipc_session, 0, 1);

  (void)host.load_graph(load_request);
  (void)host.close_graph(host_session);
  (void)host.list_graphs();
  (void)host.reload_graph(host_session, text);
  (void)host.save_graph(host_session, text);
  (void)host.clear_graph(host_session);
  (void)host.compute(compute_request);
  (void)host.compute_async(compute_request);
  (void)host.compute_and_get_image(compute_request);
  (void)host.timing(host_session);
  (void)host.last_io_time(host_session);
  (void)host.last_error(host_session);
  (void)host.list_node_ids(host_session);
  (void)host.ending_nodes(host_session);
  (void)host.get_node_yaml(host_session, node);
  (void)host.set_node_yaml(host_session, node, text);
  (void)host.inspect_node(host_session, node);
  (void)host.inspect_graph(host_session);
  (void)host.dependency_tree(host_session, std::nullopt, false);
  (void)host.traversal_orders(host_session);
  (void)host.traversal_details(host_session);
  (void)host.trees_containing_node(host_session, node);
  (void)host.project_roi(host_session, node, roi, node);
  (void)host.project_roi_backward(host_session, node, roi, node);
  (void)host.dirty_region_snapshot(host_session);
  (void)host.compute_planning_snapshot(host_session);
  (void)host.recent_compute_planning_snapshots(host_session);
  (void)host.begin_dirty_source(host_session, node, dirty_domain, roi);
  (void)host.update_dirty_source(host_session, node, dirty_domain, roi);
  (void)host.end_dirty_source(host_session, node, dirty_domain);
  (void)host.drain_compute_events(host_session, 1);
  (void)host.execution_trace(host_session, 0, 1);
  (void)host.clear_cache(host_session);
  (void)host.clear_drive_cache(host_session);
  (void)host.clear_memory_cache(host_session);
  (void)host.cache_all_nodes(host_session, text);
  (void)host.free_transient_memory(host_session);
  (void)host.synchronize_disk_cache(host_session, text);
  (void)host.plugins_load_report(paths);
  (void)host.plugins_load(paths);
  (void)host.plugins_unload_all();
  (void)host.seed_builtin_ops();
  (void)host.ops_sources();
  (void)host.ops_combined_keys();
  (void)host.ops_combined_sources();
  (void)host.policy_available_types();
  (void)host.policy_description(text);
  (void)host.policy_scan(paths);
  (void)host.policy_load(text);
  (void)host.policy_loaded_plugins();
  (void)host.configure_policy_defaults(policy_config);
  (void)host.policy_info(ps::PolicyClass::Interactive);
  (void)host.replace_policy(ps::PolicyClass::Interactive, text);
  (void)host.execution_available_types();
  (void)host.execution_description(text);
  (void)host.configure_execution_defaults(execution_config);
  (void)host.execution_info(host_session, intent);
  (void)host.replace_execution(host_session, intent, text);
  return 0;
}

}  // namespace

/**
 * @brief Exercises safe no-daemon lifecycle behavior for the installed target.
 * @param argc Process argument count; extra arguments select reference-only
 *        calls and are never supplied by the smoke test.
 * @param argv Process argument vector, unused by the no-argument smoke path.
 * @return Zero only when lifecycle and factory behavior match the contract.
 * @throws std::bad_alloc if Client or IPC Host construction exhausts memory.
 * @throws std::system_error if Host polling synchronization initialization
 *         fails.
 */
int main(int argc, char** argv) {
  (void)argv;
  ps::ipc::Client initial;
  ps::ipc::Client moved(std::move(initial));
  ps::ipc::Client client;
  client = std::move(moved);
  const ps::OperationStatus status = client.connect("");
  client.disconnect();
  client.disconnect();
  const bool disconnected = !client.connected();

  std::unique_ptr<ps::Host> host = ps::ipc::create_ipc_host("");
  if (!host) {
    return 2;
  }
  const ps::ipc::ComputeJobSnapshot default_job;
  if (default_job.cancellable) {
    return 3;
  }
  if (argc > 1) {
    return reference_complete_surface(client, *host);
  }
  return !status.ok &&
                 status.domain == ps::OperationErrorDomain::Transport &&
                 disconnected
             ? 0
             : 1;
}
"""


def complete_surface_inventory(source: str) -> dict[str, list[str]]:
    """@brief Extract the exact Client and Host call names from the harness.

    @param source Generated external IPC consumer translation unit.
    @return Ordered ``client`` and ``host`` method-name observations.
    @throws RuntimeError If the reference-only function cannot be isolated.
    @note The extraction is deliberately independent from the expected tuples,
      so deleting, duplicating, or replacing one harness call changes the
      observed count/set and fails the package behavior gate.
    """

    marker = "int reference_complete_surface"
    end_marker = "\n}\n\n}  // namespace"
    if marker not in source or end_marker not in source:
        raise RuntimeError("complete IPC surface function markers are missing")
    body = source.split(marker, 1)[1].split(end_marker, 1)[0]
    return {
        name: re.findall(rf"\b{name}\.([A-Za-z_]\w*)\s*\(", body)
        for name in ("client", "host")
    }


def validate_complete_surface_inventory(source: str) -> dict[str, list[str]]:
    """@brief Enforce exact durable Client/Host symbol-reference inventories.

    @param source Generated external IPC consumer translation unit.
    @return Extracted inventories after all exact count/set/uniqueness checks.
    @throws RuntimeError If any expected call is omitted, duplicated, or
      replaced, or if the normative expected inventories are themselves not
      exactly 60 unique Client and 58 unique Host names.
    @note This validates maintained product symbols rather than source-migration
      residue and remains part of the existing package-consumer behavior test.
    """

    expected = {
        "client": (60, EXPECTED_CLIENT_METHODS),
        "host": (58, EXPECTED_HOST_METHODS),
    }
    observed = complete_surface_inventory(source)
    failures: list[str] = []
    for label, (count, names) in expected.items():
        if len(names) != count or len(set(names)) != count:
            failures.append(f"invalid normative {label} inventory")
        if len(observed[label]) != count:
            failures.append(f"{label} call count {len(observed[label])} != {count}")
        if len(set(observed[label])) != len(observed[label]):
            failures.append(f"duplicate {label} call reference")
        if set(observed[label]) != set(names):
            missing = sorted(set(names) - set(observed[label]))
            extra = sorted(set(observed[label]) - set(names))
            failures.append(
                f"{label} call set mismatch: missing={missing}, extra={extra}"
            )
    if failures:
        raise RuntimeError("; ".join(failures))
    return observed


def write_consumer_projects(
    repo: Path, embedded_source_dir: Path, ipc_source_dir: Path
) -> tuple[list[str], dict[str, list[str]]]:
    """@brief Create independent embedded and IPC installed consumers.

    The embedded project uses default package semantics, links the static
    product, and explicitly opts into ``operation_opencv`` because its single
    translation unit compiles every installed public header, including the
    optional OpenCV adapter. The IPC project explicitly requests only the
    ``ipc_client`` component and links only its exported target. Each project
    generates its own target manifest for single- and multi-config generators.

    @param repo Repository root used only to inventory public headers.
    @param embedded_source_dir Transient embedded consumer source directory.
    @param ipc_source_dir Transient IPC-only consumer source directory.
    @return Embedded include directives and verified IPC call inventories.
    @throws OSError If directories or generated source files cannot be written.
    @throws RuntimeError If the IPC harness does not reference the exact 60
      Client and 58 Host operation names once each.
    @note The explicit adapter link supplies only this consumer's OpenCV usage
      requirements; it must not weaken the product's ``LINK_ONLY`` dependency
      contract or leak OpenCV into the operation/policy SDK consumers.
      :func:`main` recreates the surrounding work directory on every run.
    """

    embedded_source_dir.mkdir(parents=True, exist_ok=True)
    ipc_source_dir.mkdir(parents=True, exist_ok=True)
    include_lines = public_header_includes(repo)
    (embedded_source_dir / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                "project(photospider_embedded_consumer LANGUAGES CXX)",
                "find_package(Photospider CONFIG REQUIRED)",
                "if(NOT Photospider_embedded_FOUND)",
                '  message(FATAL_ERROR "default embedded component is absent")',
                "endif()",
                "add_executable(photospider_consumer main.cpp)",
                "target_link_libraries(photospider_consumer",
                "    PRIVATE",
                "        Photospider::photospider",
                "        Photospider::operation_opencv)",
                "file(GENERATE",
                '    OUTPUT "${CMAKE_BINARY_DIR}/photospider_consumer_target_$<CONFIG>.txt"',
                '    CONTENT "$<TARGET_FILE:photospider_consumer>\\n")',
                "",
            ]
        ),
        encoding="utf-8",
    )
    (embedded_source_dir / "main.cpp").write_text(
        "\n".join(
            [
                "#include <algorithm>",
                "#include <cstddef>",
                "#include <filesystem>",
                "#include <iostream>",
                "#include <memory>",
                "#include <string>",
                "#include <vector>",
                "",
                *include_lines,
                "",
                "int main(int argc, char** argv) {",
                "  if (argc != 5) {",
                '    std::cerr << "usage: consumer <policy-plugin> "',
                '                 "<operation-plugin> <session-root> <yaml>\\n";',
                "    return 64;",
                "  }",
                "  auto host = ps::create_embedded_host();",
                "  if (!host) {",
                "    return 1;",
                "  }",
                "  const auto policy_load = host->policy_load(argv[1]);",
                "  if (!policy_load.status.ok) {",
                "    std::cerr << policy_load.status.message << '\\n';",
                "    return 2;",
                "  }",
                "  const std::filesystem::path operation_plugin(argv[2]);",
                "  const auto operation_load = host->plugins_load_report(",
                "      {operation_plugin.parent_path().string()});",
                "  if (!operation_load.status.ok || operation_load.value.loaded != 1 ||",
                "      !operation_load.value.errors.empty() ||",
                "      std::find(operation_load.value.new_op_keys.begin(),",
                "                operation_load.value.new_op_keys.end(),",
                '                "installed:factory") ==',
                "          operation_load.value.new_op_keys.end()) {",
                "    std::cerr << operation_load.status.message << '\\n';",
                "    return 3;",
                "  }",
                "  ps::HostPolicyConfig policy_config;",
                '  policy_config.interactive_type = "installed_policy_smoke";',
                '  policy_config.throughput_type = "installed_policy_smoke";',
                "  const auto policy_configured =",
                "      host->configure_policy_defaults(policy_config);",
                "  if (!policy_configured.status.ok) {",
                "    std::cerr << policy_configured.status.message << '\\n';",
                "    return 4;",
                "  }",
                "  ps::HostExecutionConfig execution_config;",
                '  execution_config.hp_type = "cpu";',
                '  execution_config.rt_type = "cpu";',
                "  execution_config.worker_count = 1;",
                "  const auto execution_configured =",
                "      host->configure_execution_defaults(execution_config);",
                "  if (!execution_configured.status.ok) {",
                "    std::cerr << execution_configured.status.message << '\\n';",
                "    return 5;",
                "  }",
                "  ps::GraphLoadRequest graph_request;",
                '  graph_request.session = ps::GraphSessionId{"installed_extension"};',
                "  graph_request.root_dir = argv[3];",
                "  graph_request.yaml_path = argv[4];",
                "  graph_request.cache_root_dir =",
                '      (std::filesystem::path(argv[3]) / "cache").string();',
                "  const auto loaded = host->load_graph(graph_request);",
                "  if (!loaded.status.ok) {",
                "    std::cerr << loaded.status.message << '\\n';",
                "    return 6;",
                "  }",
                "  const auto policy_info =",
                "      host->policy_info(ps::PolicyClass::Throughput);",
                "  if (!policy_info.status.ok ||",
                '      policy_info.value.policy_type != "installed_policy_smoke") {',
                "    std::cerr << policy_info.status.message << '\\n';",
                "    return 7;",
                "  }",
                "  const auto execution_info = host->execution_info(",
                "      loaded.value, ps::ComputeIntent::GlobalHighPrecision);",
                "  if (!execution_info.status.ok ||",
                '      execution_info.value.execution_type != "cpu") {',
                "    std::cerr << execution_info.status.message << '\\n';",
                "    return 8;",
                "  }",
                "  ps::HostComputeRequest compute_request;",
                "  compute_request.session = loaded.value;",
                "  compute_request.node = ps::NodeId{1};",
                '  compute_request.cache.precision = "fp32";',
                "  compute_request.cache.nosave = true;",
                "  compute_request.execution.parallel = true;",
                "  compute_request.execution.quiet = true;",
                "  compute_request.intent = ps::ComputeIntent::GlobalHighPrecision;",
                "  const auto computed = host->compute(compute_request);",
                "  if (!computed.status.ok) {",
                "    std::cerr << computed.status.message << '\\n';",
                "    return 9;",
                "  }",
                "  const std::size_t step = ps::aligned_image_buffer_step(",
                "      8, 4, ps::DataType::FLOAT32);",
                "  auto image = ps::make_aligned_cpu_image_buffer(",
                "      2, 2, 4, ps::DataType::FLOAT32);",
                "  if (step != 128 || !image.data) {",
                "    return 10;",
                "  }",
                "  const auto closed = host->close_graph(loaded.value);",
                "  if (!closed.status.ok) {",
                "    std::cerr << closed.status.message << '\\n';",
                "    return 11;",
                "  }",
                "  const auto unloaded = host->plugins_unload_all();",
                "  if (!unloaded.status.ok || unloaded.value < 1) {",
                "    std::cerr << unloaded.status.message << '\\n';",
                "    return 12;",
                "  }",
                "  return 0;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    ipc_source = ipc_consumer_source()
    inventories = validate_complete_surface_inventory(ipc_source)
    (ipc_source_dir / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                "project(photospider_ipc_consumer LANGUAGES CXX)",
                "find_package(Photospider CONFIG",
                "    COMPONENTS ipc_client",
                "    OPTIONAL_COMPONENTS unknown_optional)",
                "if(NOT Photospider_FOUND)",
                '  message(FATAL_ERROR "ipc_client package lookup failed")',
                "endif()",
                "if(NOT Photospider_ipc_client_FOUND)",
                '  message(FATAL_ERROR "required ipc_client component is absent")',
                "endif()",
                "if(NOT TARGET Photospider::photospider_ipc_client)",
                '  message(FATAL_ERROR "required ipc_client target is absent")',
                "endif()",
                "if(Photospider_unknown_optional_FOUND)",
                '  message(FATAL_ERROR "unknown optional component was reported found")',
                "endif()",
                "if(DEFINED OpenCV_FOUND OR TARGET yaml-cpp::yaml-cpp)",
                '  message(FATAL_ERROR "IPC lookup discovered backend packages")',
                "endif()",
                "if(DEFINED PHOTOSPIDER_METAL_FRAMEWORK OR",
                "   DEFINED PHOTOSPIDER_FOUNDATION_FRAMEWORK)",
                '  message(FATAL_ERROR "IPC lookup discovered Apple backend frameworks")',
                "endif()",
                "if(NOT TARGET Threads::Threads)",
                '  message(FATAL_ERROR "IPC lookup did not resolve Threads")',
                "endif()",
                "find_package(Photospider CONFIG",
                "    COMPONENTS ipc_client OPTIONAL_COMPONENTS embedded)",
                "if(NOT Photospider_FOUND OR NOT Photospider_ipc_client_FOUND)",
                '  message(FATAL_ERROR "optional embedded probe broke IPC lookup")',
                "endif()",
                "if(Photospider_embedded_FOUND)",
                '  message(FATAL_ERROR "disabled optional embedded dependency was found")',
                "endif()",
                "add_executable(photospider_ipc_consumer main.cpp)",
                "target_link_libraries(photospider_ipc_consumer",
                "    PRIVATE Photospider::photospider_ipc_client)",
                "file(GENERATE",
                '    OUTPUT "${CMAKE_BINARY_DIR}/photospider_ipc_consumer_target_$<CONFIG>.txt"',
                '    CONTENT "$<TARGET_FILE:photospider_ipc_consumer>\\n")',
                "",
            ]
        ),
        encoding="utf-8",
    )
    (ipc_source_dir / "main.cpp").write_text(ipc_source, encoding="utf-8")
    return include_lines, inventories


def installed_policy_plugin_source() -> str:
    """@brief Build a source-tree-independent pure-C policy DSO consumer.

    @return C11 source implementing and probing the complete policy ABI v1.
    @throws None The source is one immutable in-memory string.
    @note The policy SDK is one dependency-neutral header and exposes no Host
      execution, worker, resource, Graph, Run, or lifecycle capability.
    """

    return dedent(
        r"""
        #include <stddef.h>
        #include <stdint.h>

        #include <photospider/policy/policy_plugin_api.h>

        _Static_assert(PS_POLICY_PLUGIN_ABI_VERSION == 1U,
                       "policy ABI generation changed");
        _Static_assert(sizeof(ps_policy_candidate_v1) == 120U,
                       "policy candidate layout changed");

        /**
         * @brief Builds a borrowed immutable metadata range.
         * @param data Process-lifetime bytes.
         * @param size Byte count excluding the terminator.
         * @return ABI-v1 string view over the supplied storage.
         */
        static ps_policy_string_view_v1 installed_string_view(
            const char *data, uint64_t size) {
          ps_policy_string_view_v1 view;
          view.data = data;
          view.size = size;
          return view;
        }

        /**
         * @brief Returns the single installed smoke policy metadata row.
         * @param type_index Zero-based row; only zero is valid.
         * @param out_metadata Nonnull exact-size output record.
         * @return OK or INVALID_ARGUMENT.
         */
        static ps_policy_status_v1 PS_POLICY_CALL installed_get_metadata(
            uint32_t type_index, ps_policy_type_metadata_v1 *out_metadata) {
          static const char kName[] = "installed_policy_smoke";
          static const char kDescription[] =
              "Installed pure-C policy SDK smoke fixture.";
          static const char kVersion[] = "installed-smoke-v1";
          if (type_index != 0U || out_metadata == NULL) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_metadata = (ps_policy_type_metadata_v1){0};
          out_metadata->struct_size = sizeof(*out_metadata);
          out_metadata->struct_kind = PS_POLICY_STRUCT_TYPE_METADATA;
          out_metadata->name =
              installed_string_view(kName, sizeof(kName) - 1U);
          out_metadata->description =
              installed_string_view(kDescription, sizeof(kDescription) - 1U);
          out_metadata->implementation_version =
              installed_string_view(kVersion, sizeof(kVersion) - 1U);
          out_metadata->supported_class_mask = PS_POLICY_CLASS_MASK_VALID;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Creates one stateless class-specific policy context.
         * @param type_index Zero-based row; only zero is valid.
         * @param args Nonnull exact ABI-v1 creation arguments.
         * @param out_context Nonnull context output initialized to null.
         * @return OK for a valid class binding, otherwise INVALID_ARGUMENT.
         */
        static ps_policy_status_v1 PS_POLICY_CALL installed_create(
            uint32_t type_index, const ps_policy_create_args_v1 *args,
            void **out_context) {
          if (type_index != 0U || args == NULL || out_context == NULL ||
              args->struct_size != sizeof(*args) ||
              args->struct_kind != PS_POLICY_STRUCT_CREATE_ARGS ||
              args->binding_generation == 0U ||
              (args->policy_class != PS_POLICY_CLASS_INTERACTIVE &&
               args->policy_class != PS_POLICY_CLASS_THROUGHPUT)) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_context = NULL;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Selects the final candidate from one immutable snapshot.
         * @param context Stateless context; ignored.
         * @param snapshot Nonnull exact-size snapshot with candidates.
         * @param out_decision Nonnull exact-size output record.
         * @return OK for a valid snapshot, otherwise INVALID_ARGUMENT.
         * @note No input pointer is retained beyond the callback.
         */
        static ps_policy_status_v1 PS_POLICY_CALL installed_select(
            void *context, const ps_policy_selection_snapshot_v1 *snapshot,
            ps_policy_decision_v1 *out_decision) {
          (void)context;
          if (snapshot == NULL || out_decision == NULL ||
              snapshot->struct_size != sizeof(*snapshot) ||
              snapshot->struct_kind != PS_POLICY_STRUCT_SELECTION_SNAPSHOT ||
              snapshot->candidate_count == 0U ||
              snapshot->candidate_stride != sizeof(ps_policy_candidate_v1) ||
              snapshot->candidates == NULL ||
              snapshot->binding_generation == 0U ||
              snapshot->snapshot_generation == 0U) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_decision = (ps_policy_decision_v1){0};
          out_decision->struct_size = sizeof(*out_decision);
          out_decision->struct_kind = PS_POLICY_STRUCT_DECISION;
          out_decision->decision_kind = PS_POLICY_DECISION_SELECT;
          out_decision->binding_generation = snapshot->binding_generation;
          out_decision->snapshot_generation = snapshot->snapshot_generation;
          out_decision->candidate_id =
              snapshot->candidates[snapshot->candidate_count - 1U].candidate_id;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Completes one successful stateless context lifetime.
         * @param context Stateless context; ignored, including null.
         * @return Always OK.
         */
        static ps_policy_status_v1 PS_POLICY_CALL installed_destroy(
            void *context) {
          (void)context;
          return PS_POLICY_STATUS_OK;
        }

        /** @copydoc ps_policy_plugin_get_abi_version */
        PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
        ps_policy_plugin_get_abi_version(void) {
          return PS_POLICY_PLUGIN_ABI_VERSION;
        }

        /** @copydoc ps_policy_plugin_get_api_v1 */
        PS_POLICY_PLUGIN_EXPORT ps_policy_status_v1 PS_POLICY_CALL
        ps_policy_plugin_get_api_v1(ps_policy_plugin_api_v1 *out_api) {
          if (out_api == NULL) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_api = (ps_policy_plugin_api_v1){0};
          out_api->struct_size = sizeof(*out_api);
          out_api->struct_kind = PS_POLICY_STRUCT_PLUGIN_API;
          out_api->abi_version = PS_POLICY_PLUGIN_ABI_VERSION;
          out_api->type_count = 1U;
          out_api->get_metadata = installed_get_metadata;
          out_api->create = installed_create;
          out_api->select = installed_select;
          out_api->destroy = installed_destroy;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Exercises the installed API table without backend symbols.
         * @return Zero on success or a stable nonzero probe stage.
         * @note This fixture-only export is not part of the policy ABI.
         */
        PS_POLICY_PLUGIN_EXPORT int PS_POLICY_CALL
        ps_installed_policy_contract_probe(void) {
          ps_policy_plugin_api_v1 api = {0};
          ps_policy_type_metadata_v1 metadata = {0};
          ps_policy_create_args_v1 create_args = {0};
          ps_policy_candidate_v1 candidates[2] = {{0}, {0}};
          ps_policy_selection_snapshot_v1 snapshot = {0};
          ps_policy_decision_v1 decision = {0};
          void *context = NULL;

          if (ps_policy_plugin_get_abi_version() != 1U ||
              ps_policy_plugin_get_api_v1(&api) != PS_POLICY_STATUS_OK ||
              api.struct_size != sizeof(api) || api.type_count != 1U) {
            return 1;
          }
          if (api.get_metadata(0U, &metadata) != PS_POLICY_STATUS_OK ||
              metadata.name.size != 22U ||
              metadata.supported_class_mask != PS_POLICY_CLASS_MASK_VALID) {
            return 2;
          }
          create_args.struct_size = sizeof(create_args);
          create_args.struct_kind = PS_POLICY_STRUCT_CREATE_ARGS;
          create_args.policy_class = PS_POLICY_CLASS_INTERACTIVE;
          create_args.binding_generation = 7U;
          if (api.create(0U, &create_args, &context) != PS_POLICY_STATUS_OK ||
              context != NULL) {
            return 3;
          }
          candidates[0].struct_size = sizeof(candidates[0]);
          candidates[0].struct_kind = PS_POLICY_STRUCT_CANDIDATE;
          candidates[0].candidate_id = 11U;
          candidates[1].struct_size = sizeof(candidates[1]);
          candidates[1].struct_kind = PS_POLICY_STRUCT_CANDIDATE;
          candidates[1].candidate_id = 19U;
          snapshot.struct_size = sizeof(snapshot);
          snapshot.struct_kind = PS_POLICY_STRUCT_SELECTION_SNAPSHOT;
          snapshot.policy_class = PS_POLICY_CLASS_INTERACTIVE;
          snapshot.candidate_count = 2U;
          snapshot.binding_generation = 7U;
          snapshot.snapshot_generation = 13U;
          snapshot.selection_sequence = 1U;
          snapshot.candidate_stride = sizeof(ps_policy_candidate_v1);
          snapshot.candidates = candidates;
          if (api.select(context, &snapshot, &decision) != PS_POLICY_STATUS_OK ||
              decision.decision_kind != PS_POLICY_DECISION_SELECT ||
              decision.binding_generation != 7U ||
              decision.snapshot_generation != 13U ||
              decision.candidate_id != 19U) {
            return 4;
          }
          return api.destroy(context) == PS_POLICY_STATUS_OK ? 0 : 5;
        }
        """
    ).lstrip()


def installed_cpp_policy_plugin_source() -> str:
    """@brief Build a source-tree-independent C++17 policy DSO consumer.

    @return C++17 source implementing and probing the complete policy ABI v1.
    @throws None The source is one immutable in-memory string.
    @note Every ABI callback is explicitly ``noexcept`` and the DSO links only
      the dependency-neutral installed ``policy_sdk`` interface target.
    """

    return dedent(
        r"""
        #include <cstddef>
        #include <cstdint>
        #include <type_traits>

        #include <photospider/policy/policy_plugin_api.h>

        static_assert(PS_POLICY_PLUGIN_ABI_VERSION == 1U,
                      "policy ABI generation changed");
        static_assert(sizeof(ps_policy_candidate_v1) == 120U,
                      "policy candidate layout changed");
        static_assert(std::is_nothrow_invocable_r_v<
                          ps_policy_status_v1, ps_policy_select_fn_v1, void*,
                          const ps_policy_selection_snapshot_v1*,
                          ps_policy_decision_v1*>,
                      "C++ callback profile must remain noexcept");

        namespace {

        /**
         * @brief Builds one borrowed immutable metadata range.
         * @param data Process-lifetime bytes.
         * @param size Byte count excluding the terminator.
         * @return ABI-v1 string view over the supplied storage.
         * @throws Nothing.
         */
        ps_policy_string_view_v1 string_view(const char* data,
                                             std::uint64_t size) noexcept {
          return ps_policy_string_view_v1{data, size};
        }

        /**
         * @brief Returns the single installed C++17 policy metadata row.
         * @param type_index Zero-based row; only zero is valid.
         * @param out_metadata Nonnull exact-size output record.
         * @return OK or INVALID_ARGUMENT.
         * @throws Nothing.
         */
        ps_policy_status_v1 PS_POLICY_CALL get_metadata(
            std::uint32_t type_index,
            ps_policy_type_metadata_v1* out_metadata) noexcept {
          static constexpr char kName[] = "installed_policy_smoke";
          static constexpr char kDescription[] =
              "Installed C++17 policy SDK smoke fixture.";
          static constexpr char kVersion[] = "installed-cpp17-v1";
          if (type_index != 0U || out_metadata == nullptr) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_metadata = {};
          out_metadata->struct_size = sizeof(*out_metadata);
          out_metadata->struct_kind = PS_POLICY_STRUCT_TYPE_METADATA;
          out_metadata->name = string_view(kName, sizeof(kName) - 1U);
          out_metadata->description =
              string_view(kDescription, sizeof(kDescription) - 1U);
          out_metadata->implementation_version =
              string_view(kVersion, sizeof(kVersion) - 1U);
          out_metadata->supported_class_mask = PS_POLICY_CLASS_MASK_VALID;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Creates one stateless class-specific logical instance.
         * @param type_index Zero-based row; only zero is valid.
         * @param args Nonnull exact ABI-v1 creation arguments.
         * @param out_context Nonnull context output initialized to null.
         * @return OK for one valid binding, otherwise INVALID_ARGUMENT.
         * @throws Nothing.
         */
        ps_policy_status_v1 PS_POLICY_CALL create(
            std::uint32_t type_index, const ps_policy_create_args_v1* args,
            void** out_context) noexcept {
          if (type_index != 0U || args == nullptr || out_context == nullptr ||
              args->struct_size != sizeof(*args) ||
              args->struct_kind != PS_POLICY_STRUCT_CREATE_ARGS ||
              args->binding_generation == 0U || args->reserved0 != 0U ||
              args->reserved[0] != 0U || args->reserved[1] != 0U ||
              (args->policy_class != PS_POLICY_CLASS_INTERACTIVE &&
               args->policy_class != PS_POLICY_CLASS_THROUGHPUT)) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_context = nullptr;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Selects the first candidate from one immutable snapshot.
         * @param context Stateless context; ignored, including null.
         * @param snapshot Nonnull exact-size snapshot with candidates.
         * @param out_decision Nonnull exact-size output record.
         * @return OK for a valid snapshot, otherwise INVALID_ARGUMENT.
         * @throws Nothing.
         */
        ps_policy_status_v1 PS_POLICY_CALL select(
            void* context, const ps_policy_selection_snapshot_v1* snapshot,
            ps_policy_decision_v1* out_decision) noexcept {
          (void)context;
          if (snapshot == nullptr || out_decision == nullptr ||
              snapshot->struct_size != sizeof(*snapshot) ||
              snapshot->struct_kind != PS_POLICY_STRUCT_SELECTION_SNAPSHOT ||
              snapshot->candidate_count == 0U ||
              snapshot->candidate_stride != sizeof(ps_policy_candidate_v1) ||
              snapshot->candidates == nullptr ||
              snapshot->binding_generation == 0U ||
              snapshot->snapshot_generation == 0U ||
              snapshot->reserved0 != 0U || snapshot->reserved[0] != 0U) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_decision = {};
          out_decision->struct_size = sizeof(*out_decision);
          out_decision->struct_kind = PS_POLICY_STRUCT_DECISION;
          out_decision->decision_kind = PS_POLICY_DECISION_SELECT;
          out_decision->binding_generation = snapshot->binding_generation;
          out_decision->snapshot_generation = snapshot->snapshot_generation;
          out_decision->candidate_id = snapshot->candidates[0].candidate_id;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Completes one successful stateless logical instance.
         * @param context Stateless context; ignored, including null.
         * @return Always OK.
         * @throws Nothing.
         */
        ps_policy_status_v1 PS_POLICY_CALL destroy(void* context) noexcept {
          (void)context;
          return PS_POLICY_STATUS_OK;
        }

        }  // namespace

        /** @copydoc ps_policy_plugin_get_abi_version */
        PS_POLICY_PLUGIN_EXPORT std::uint32_t PS_POLICY_CALL
        ps_policy_plugin_get_abi_version(void) noexcept {
          return PS_POLICY_PLUGIN_ABI_VERSION;
        }

        /** @copydoc ps_policy_plugin_get_api_v1 */
        PS_POLICY_PLUGIN_EXPORT ps_policy_status_v1 PS_POLICY_CALL
        ps_policy_plugin_get_api_v1(
            ps_policy_plugin_api_v1* out_api) noexcept {
          if (out_api == nullptr) {
            return PS_POLICY_STATUS_INVALID_ARGUMENT;
          }
          *out_api = {};
          out_api->struct_size = sizeof(*out_api);
          out_api->struct_kind = PS_POLICY_STRUCT_PLUGIN_API;
          out_api->abi_version = PS_POLICY_PLUGIN_ABI_VERSION;
          out_api->type_count = 1U;
          out_api->get_metadata = get_metadata;
          out_api->create = create;
          out_api->select = select;
          out_api->destroy = destroy;
          return PS_POLICY_STATUS_OK;
        }

        /**
         * @brief Exercises the installed C++17 ABI table without Host linkage.
         * @return Zero only when create/select/destroy and echoes are exact.
         * @throws Nothing.
         * @note This fixture-only export is not part of the policy ABI.
         */
        extern "C" PS_POLICY_PLUGIN_EXPORT int PS_POLICY_CALL
        ps_installed_policy_contract_probe(void) noexcept {
          ps_policy_plugin_api_v1 api{};
          ps_policy_create_args_v1 create_args{};
          ps_policy_candidate_v1 candidate{};
          ps_policy_selection_snapshot_v1 snapshot{};
          ps_policy_decision_v1 decision{};
          void* context = nullptr;
          if (ps_policy_plugin_get_api_v1(&api) != PS_POLICY_STATUS_OK ||
              api.struct_size != sizeof(api) || api.type_count != 1U) {
            return 1;
          }
          create_args.struct_size = sizeof(create_args);
          create_args.struct_kind = PS_POLICY_STRUCT_CREATE_ARGS;
          create_args.policy_class = PS_POLICY_CLASS_INTERACTIVE;
          create_args.binding_generation = 17U;
          if (api.create(0U, &create_args, &context) != PS_POLICY_STATUS_OK) {
            return 2;
          }
          candidate.struct_size = sizeof(candidate);
          candidate.struct_kind = PS_POLICY_STRUCT_CANDIDATE;
          candidate.candidate_id = 23U;
          snapshot.struct_size = sizeof(snapshot);
          snapshot.struct_kind = PS_POLICY_STRUCT_SELECTION_SNAPSHOT;
          snapshot.policy_class = PS_POLICY_CLASS_INTERACTIVE;
          snapshot.candidate_count = 1U;
          snapshot.binding_generation = 17U;
          snapshot.snapshot_generation = 19U;
          snapshot.selection_sequence = 1U;
          snapshot.candidate_stride = sizeof(candidate);
          snapshot.candidates = &candidate;
          if (api.select(context, &snapshot, &decision) !=
                  PS_POLICY_STATUS_OK ||
              decision.candidate_id != 23U ||
              decision.binding_generation != 17U ||
              decision.snapshot_generation != 19U) {
            return 3;
          }
          return api.destroy(context) == PS_POLICY_STATUS_OK ? 0 : 4;
        }
        """
    ).lstrip()


def installed_operation_plugin_source() -> str:
    """@brief Build an operation DSO that needs only ``operation_sdk``.

    @return C++17 source registering one factory-backed monolithic operation.
    @throws None The source is one immutable in-memory string.
    @note Calling the image factory from the DSO forces the SDK's transitive
      runtime archive to satisfy the symbol at link time.
    """

    return dedent(
        r"""
        #include <stdexcept>
        #include <string_view>
        #include <type_traits>

        #include <photospider/plugin/plugin_api.hpp>

        /**
         * @brief Detects an added operation-registrar cancellation shim.
         * @tparam T Installed registrar contract under inspection.
         * @tparam Probe Substitution-only member-address expression.
         * @throws Nothing.
         * @note Operation ABI v2 receives callbacks only; it owns no Run.
         */
        template <typename T, typename Probe = void>
        struct HasOperationCancel : std::false_type {};

        /** @copydoc HasOperationCancel */
        template <typename T>
        struct HasOperationCancel<T, std::void_t<decltype(&T::cancel)>>
            : std::true_type {};

        static_assert(!HasOperationCancel<
                          ps::plugin::OperationPluginRegistrar>::value,
                      "operation ABI v2 must expose no cancellation shim");
        static_assert(std::is_standard_layout_v<
                          ps::plugin::OperationPluginRegistrar>,
                      "operation ABI v2 registrar must remain standard layout");
        static_assert(
            sizeof(ps::plugin::OperationPluginRegistrar) == 9U * sizeof(void*),
            "operation ABI v2 registrar pointer-table layout changed");
        static_assert(
            std::string_view(ps::plugin::kOperationPluginRegisterSymbolV2) ==
                "register_photospider_ops_v2",
            "operation plugin handshake must remain version two");

        /**
         * @brief Registers one installed-SDK operation using the image factory.
         * @param registrar Borrowed host registration transaction.
         * @return Nothing.
         * @throws std::invalid_argument for a null registrar.
         * @throws Any allocation or host registration exception unchanged.
         */
        extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void
        register_photospider_ops_v2(
            ps::plugin::OperationPluginRegistrar* registrar) {
          if (registrar == nullptr) {
            throw std::invalid_argument("installed operation registrar is null");
          }
          registrar->register_op_hp_monolithic(
              "installed", "factory",
              [](const ps::plugin::NodeView&,
                 ps::plugin::ArrayView<ps::plugin::OperationInputView>) {
                ps::plugin::OperationOutput output;
                output.image_buffer = ps::make_aligned_cpu_image_buffer(
                    2, 2, 1, ps::DataType::UINT8);
                return output;
              });
        }
        """
    ).lstrip()


def write_extension_consumer_projects(
    policy_source_dir: Path,
    operation_source_dir: Path,
    opencv_source_dir: Path,
    real_opencv_config_dir: Path,
) -> None:
    """@brief Create three independent installed extension-SDK consumers.

    @param policy_source_dir Pure-C/C++17 policy DSO project directory.
    @param operation_source_dir Operation SDK DSO/executable project directory.
    @param opencv_source_dir OpenCV-core-only adapter consumer directory.
    @param real_opencv_config_dir Producer-resolved OpenCV config directory.
    @return Nothing.
    @throws OSError If project or shim files cannot be written.
    @throws RuntimeError If the real OpenCV package config cannot be located.
    @note No generated target receives a repository source include directory.
      The operation fixture is a ``SHARED`` library so its platform suffix
      matches the production directory scanner (``.dylib`` on macOS); the
      both policy fixtures are ``SHARED`` libraries accepted by the production
      loader. The policy project must also reject a packed C++17 ABI profile.
    """

    policy_source_dir.mkdir(parents=True, exist_ok=True)
    operation_source_dir.mkdir(parents=True, exist_ok=True)
    opencv_source_dir.mkdir(parents=True, exist_ok=True)

    (policy_source_dir / "CMakeLists.txt").write_text(
        dedent(
            """
            cmake_minimum_required(VERSION 3.16)
            project(installed_policy_sdk_consumer LANGUAGES C CXX)
            find_package(Photospider CONFIG REQUIRED COMPONENTS policy_sdk)
            if(DEFINED OpenCV_FOUND OR TARGET yaml-cpp::yaml-cpp OR
               TARGET Threads::Threads)
              message(FATAL_ERROR "policy SDK discovered backend packages")
            endif()
            add_library(installed_policy_plugin SHARED policy_plugin.c)
            set_target_properties(installed_policy_plugin PROPERTIES
                C_STANDARD 11
                C_STANDARD_REQUIRED YES)
            target_link_libraries(installed_policy_plugin PRIVATE
                Photospider::policy_sdk)
            add_library(installed_cpp_policy_plugin SHARED policy_plugin.cpp)
            set_target_properties(installed_cpp_policy_plugin PROPERTIES
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED YES
                CXX_EXTENSIONS NO)
            target_link_libraries(installed_cpp_policy_plugin PRIVATE
                Photospider::policy_sdk)
            get_target_property(POLICY_SDK_INCLUDE_DIRS
                Photospider::policy_sdk INTERFACE_INCLUDE_DIRECTORIES)
            set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
            try_compile(PACKED_POLICY_PROFILE_COMPILED
                "${CMAKE_BINARY_DIR}/packed-policy-profile-build"
                "${CMAKE_CURRENT_SOURCE_DIR}/packed_profile.cpp"
                CMAKE_FLAGS
                    "-DCMAKE_CXX_STANDARD:STRING=17"
                    "-DCMAKE_CXX_STANDARD_REQUIRED:BOOL=ON"
                    "-DCMAKE_CXX_EXTENSIONS:BOOL=OFF"
                    "-DINCLUDE_DIRECTORIES:STRING=${POLICY_SDK_INCLUDE_DIRS}"
                OUTPUT_VARIABLE PACKED_POLICY_PROFILE_OUTPUT)
            unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
            if(PACKED_POLICY_PROFILE_COMPILED)
              message(FATAL_ERROR
                  "packed C++17 policy ABI profile unexpectedly compiled")
            endif()
            file(GENERATE
                OUTPUT "${CMAKE_BINARY_DIR}/installed_policy_plugin_target_$<CONFIG>.txt"
                CONTENT "$<TARGET_FILE:installed_policy_plugin>\n")
            file(GENERATE
                OUTPUT "${CMAKE_BINARY_DIR}/installed_cpp_policy_plugin_target_$<CONFIG>.txt"
                CONTENT "$<TARGET_FILE:installed_cpp_policy_plugin>\n")
            """
        ).lstrip(),
        encoding="utf-8",
    )
    (policy_source_dir / "policy_plugin.c").write_text(
        installed_policy_plugin_source(), encoding="utf-8"
    )
    (policy_source_dir / "policy_plugin.cpp").write_text(
        installed_cpp_policy_plugin_source(), encoding="utf-8"
    )
    (policy_source_dir / "packed_profile.cpp").write_text(
        dedent(
            """
            #pragma pack(push, 1)
            #include <photospider/policy/policy_plugin_api.h>
            #pragma pack(pop)

            int main() { return 0; }
            """
        ).lstrip(),
        encoding="utf-8",
    )

    (operation_source_dir / "CMakeLists.txt").write_text(
        dedent(
            """
            cmake_minimum_required(VERSION 3.16)
            project(installed_operation_sdk_consumer LANGUAGES CXX)
            find_package(Photospider CONFIG REQUIRED COMPONENTS operation_sdk)
            if(DEFINED OpenCV_FOUND OR TARGET yaml-cpp::yaml-cpp OR
               TARGET Threads::Threads)
              message(FATAL_ERROR "operation SDK discovered backend packages")
            endif()
            # The Host scans operation-plugin directories by the documented
            # platform shared-library suffix, including .dylib on macOS.
            add_library(installed_operation_plugin SHARED operation_plugin.cpp)
            target_compile_definitions(installed_operation_plugin PRIVATE
                PHOTOSPIDER_PLUGIN_BUILD)
            target_link_libraries(installed_operation_plugin PRIVATE
                Photospider::operation_sdk)
            file(GENERATE
                OUTPUT "${CMAKE_BINARY_DIR}/installed_operation_plugin_target_$<CONFIG>.txt"
                CONTENT "$<TARGET_FILE:installed_operation_plugin>\n")
            add_executable(installed_operation_factory main.cpp)
            target_link_libraries(installed_operation_factory PRIVATE
                Photospider::operation_sdk)
            file(GENERATE
                OUTPUT "${CMAKE_BINARY_DIR}/installed_operation_factory_target_$<CONFIG>.txt"
                CONTENT "$<TARGET_FILE:installed_operation_factory>\n")
            """
        ).lstrip(),
        encoding="utf-8",
    )
    (operation_source_dir / "operation_plugin.cpp").write_text(
        installed_operation_plugin_source(), encoding="utf-8"
    )
    (operation_source_dir / "main.cpp").write_text(
        dedent(
            """
            #include <photospider/core/image_buffer.hpp>

            /**
             * @brief Calls the SDK-transitive image factory from an executable.
             * @return Zero only when allocated storage and dimensions are valid.
             * @throws Nothing; allocation failure terminates the smoke process.
             */
            int main() {
              const auto image = ps::make_aligned_cpu_image_buffer(
                  3, 2, 1, ps::DataType::UINT8);
              return image.data && image.width == 3 && image.height == 2 ? 0 : 1;
            }
            """
        ).lstrip(),
        encoding="utf-8",
    )

    real_config_candidates = (
        real_opencv_config_dir / "OpenCVConfig.cmake",
        real_opencv_config_dir / "opencv-config.cmake",
    )
    real_config = next(
        (candidate for candidate in real_config_candidates if candidate.is_file()),
        None,
    )
    if real_config is None:
        raise RuntimeError(f"OpenCV config not found below {real_opencv_config_dir}")
    shim_dir = opencv_source_dir / "opencv-core-shim"
    shim_dir.mkdir()
    (shim_dir / "OpenCVConfig.cmake").write_text(
        dedent(
            f"""
            if(NOT "${{OpenCV_FIND_COMPONENTS}}" STREQUAL "core")
              message(FATAL_ERROR
                  "operation_opencv requested non-core modules: ${{OpenCV_FIND_COMPONENTS}}")
            endif()
            include("{real_config.as_posix()}")
            """
        ).lstrip(),
        encoding="utf-8",
    )
    (opencv_source_dir / "CMakeLists.txt").write_text(
        dedent(
            """
            cmake_minimum_required(VERSION 3.16)
            project(installed_operation_opencv_consumer LANGUAGES CXX)
            find_package(Photospider CONFIG REQUIRED COMPONENTS operation_opencv)
            if(TARGET yaml-cpp::yaml-cpp OR TARGET Threads::Threads)
              message(FATAL_ERROR "operation_opencv discovered unrelated packages")
            endif()
            add_executable(installed_operation_opencv main.cpp)
            target_link_libraries(installed_operation_opencv PRIVATE
                Photospider::operation_opencv)
            file(GENERATE
                OUTPUT "${CMAKE_BINARY_DIR}/installed_operation_opencv_target_$<CONFIG>.txt"
                CONTENT "$<TARGET_FILE:installed_operation_opencv>\n")
            """
        ).lstrip(),
        encoding="utf-8",
    )
    (opencv_source_dir / "main.cpp").write_text(
        dedent(
            """
            #include <opencv2/core.hpp>
            #include <photospider/plugin/opencv_adapter.hpp>

            /**
             * @brief Exercises the installed adapter using OpenCV core only.
             * @return Zero when the wrapped descriptor retains matrix storage.
             * @throws Nothing; adapter failure terminates the smoke process.
             */
            int main() {
              cv::Mat matrix(2, 3, CV_8UC1, cv::Scalar(9));
              const auto image = ps::plugin::opencv::from_mat(matrix);
              return image.data && image.width == 3 && image.height == 2 ? 0 : 1;
            }
            """
        ).lstrip(),
        encoding="utf-8",
    )


def write_missing_opencv_component_projects(
    optional_source_dir: Path, required_source_dir: Path
) -> None:
    """@brief Create OpenCV-missing optional and required package probes.

    @param optional_source_dir Consumer requiring the dependency-free operation
      SDK while requesting ``operation_opencv`` as an optional component.
    @param required_source_dir Consumer requiring ``operation_opencv``.
    @return Nothing.
    @throws OSError If either transient CMake project cannot be written.
    @note Both projects are configured with OpenCV discovery disabled by the
      caller. The optional probe also verifies that usable SDK targets remain
      imported while the unavailable OpenCV adapter target stays hidden.
    """

    optional_source_dir.mkdir(parents=True, exist_ok=True)
    required_source_dir.mkdir(parents=True, exist_ok=True)
    (optional_source_dir / "CMakeLists.txt").write_text(
        dedent(
            """
            cmake_minimum_required(VERSION 3.16)
            project(optional_operation_opencv_consumer LANGUAGES NONE)
            find_package(Photospider CONFIG
                COMPONENTS operation_sdk
                OPTIONAL_COMPONENTS operation_opencv)
            if(NOT Photospider_FOUND OR NOT Photospider_operation_sdk_FOUND)
              message(FATAL_ERROR "required operation_sdk lookup failed")
            endif()
            if(Photospider_operation_opencv_FOUND)
              message(FATAL_ERROR "missing optional operation_opencv was found")
            endif()
            if(TARGET Photospider::operation_opencv)
              message(FATAL_ERROR "missing optional operation_opencv target was exposed")
            endif()
            if(NOT TARGET Photospider::operation_sdk OR
               NOT TARGET Photospider::operation_runtime)
              message(FATAL_ERROR "available operation SDK targets were not imported")
            endif()
            add_library(optional_operation_opencv_consumer INTERFACE)
            target_link_libraries(optional_operation_opencv_consumer INTERFACE
                Photospider::operation_sdk)
            """
        ).lstrip(),
        encoding="utf-8",
    )
    (required_source_dir / "CMakeLists.txt").write_text(
        dedent(
            """
            cmake_minimum_required(VERSION 3.16)
            project(required_operation_opencv_consumer LANGUAGES NONE)
            find_package(Photospider CONFIG REQUIRED
                COMPONENTS operation_opencv)
            """
        ).lstrip(),
        encoding="utf-8",
    )


def cmake_cache_value(build: Path, key: str) -> str:
    """@brief Read one exact entry from a configured CMake cache.

    @param build Build directory that may contain ``CMakeCache.txt``.
    @param key Cache variable name before the CMake type separator.
    @return Value after ``=`` or an empty string when absent/unconfigured.
    @throws OSError If an existing cache cannot be read.
    @note The cache is read-only; type information is intentionally ignored.
    """

    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if not line.startswith(prefix):
            continue
        return line.split("=", 1)[1] if "=" in line else ""
    return ""


def installed_headers(prefix: Path) -> list[str]:
    """@brief Inventory files installed under the include root.

    @param prefix Temporary package installation prefix.
    @return Sorted paths relative to ``prefix``; missing include roots yield ``[]``.
    @throws OSError If an existing include tree cannot be traversed.
    @note All file types are returned so unexpected non-header payloads cannot be
      hidden by a suffix filter.
    """

    include_dir = prefix / "include"
    if not include_dir.exists():
        return []
    return sorted(
        path.relative_to(prefix).as_posix()
        for path in include_dir.rglob("*")
        if path.is_file()
    )


def installed_static_product_archives(prefix: Path, install_libdir: str) -> list[str]:
    """@brief Find platform-specific Photospider static archives.

    @param prefix Temporary package installation prefix.
    @param install_libdir Configured GNUInstallDirs library directory.
    @return Sorted archive paths relative to ``prefix``.
    @throws OSError If an existing library tree cannot be traversed.
    @note Archive names cover Unix-like and MSVC conventions; the function does
      not accept shared-library suffixes.
    """

    archive_root = prefix / install_libdir
    if not archive_root.exists():
        return []
    return sorted(
        path.relative_to(prefix).as_posix()
        for path in archive_root.rglob("*")
        if path.is_file() and path.name.lower() in STATIC_PRODUCT_ARCHIVE_NAMES
    )


def inspect_product_archive_symbols(
    prefix: Path,
    archives: list[str],
    platform_system: str,
    *,
    which: Callable[[str], str | None] = shutil.which,
    captured_runner: Callable[
        [list[str], Path], subprocess.CompletedProcess[str]
    ] = run_captured_command,
    executable_validator: Callable[[str], bool] = is_executable_file,
) -> dict[str, Any]:
    """@brief Inspect the installed product archive for private test seams.

    @param prefix Temporary package installation prefix.
    @param archives Product archive paths relative to ``prefix``.
    @param platform_system Host platform controlling Darwin ``xcrun`` use.
    @param which Injectable executable lookup for deterministic regression.
    @param captured_runner Injectable captured subprocess runner used for both
      discovery and archive scans.
    @param executable_validator Injectable validator for ``xcrun`` output.
    @return Path-free symbol-tool source/status, aggregate line/anchor/prohibited
      counts, controlled symbol tokens, and structured candidate diagnostics. A
      missing/non-unique archive or all unusable tools produces an unsuccessful
      observation with the same closed schema.
    @throws None Process startup failures are converted to candidate failures;
      injected callbacks are otherwise expected to honor their contracts.
    @note Darwin tries validated Xcode ``xcrun`` llvm-nm, PATH llvm-nm, then
      PATH nm; other platforms never invoke xcrun. A candidate becomes
      authoritative only after a zero exit, nonempty symbol table, and defined
      anchors from all five seam objects. Raw lines remain only in this stack
      frame: once authoritative, controlled fragment counts are retained and no
      later tool may hide a forbidden symbol.
    """

    if len(archives) != 1:
        return {
            "tool_source": "",
            "status": None,
            "line_count": 0,
            "prohibited_symbol_count": 0,
            "prohibited_symbols": {},
            "required_anchor_count": 0,
            "required_anchor_total": len(
                REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
            ),
            "required_anchors": {
                fragment: 0
                for fragment in REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
            },
            "covers_product_seams": False,
            "attempts": [],
            "failure_reason": (
                "expected exactly one installed product archive"
            ),
        }
    resolution = resolve_product_archive_symbol_tools(
        platform_system,
        prefix,
        which=which,
        captured_runner=captured_runner,
        executable_validator=executable_validator,
    )
    attempts: list[dict[str, Any]] = []
    last_status: int | None = None
    last_line_count = 0
    last_prohibited_symbols: dict[str, int] = {}
    last_required_anchors = {
        fragment: 0 for fragment in REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
    }
    for entry in resolution.sequence:
        if isinstance(entry, SymbolToolFailure):
            attempt: dict[str, Any] = {
                "tool_source": entry.source,
                "reason": entry.reason,
            }
            if entry.status is not None:
                attempt["status"] = entry.status
            attempts.append(attempt)
            continue
        candidate = entry
        try:
            completed = captured_runner(
                [candidate.executable, str(prefix / archives[0])], prefix
            )
        except OSError:
            attempts.append(
                {
                    "tool_source": candidate.source,
                    "reason": "inspection could not start",
                }
            )
            continue
        symbol_lines = completed.stdout.splitlines()
        prohibited_symbols = {
            fragment: sum(fragment in line for line in symbol_lines)
            for fragment in FORBIDDEN_PRODUCT_TEST_SYMBOL_FRAGMENTS
        }
        required_anchors = {
            fragment: sum(
                1
                for line in symbol_lines
                if fragment in line and re.search(r"\b[TW]\b", line)
            )
            for fragment in REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
        }
        last_status = completed.returncode
        last_line_count = len(symbol_lines)
        last_prohibited_symbols = {
            fragment: count
            for fragment, count in prohibited_symbols.items()
            if count
        }
        last_required_anchors = required_anchors
        missing_anchor_count = sum(
            count == 0 for count in required_anchors.values()
        )
        if completed.returncode != 0:
            attempts.append(
                {
                    "tool_source": candidate.source,
                    "reason": "inspection exited with nonzero status",
                    "status": completed.returncode,
                }
            )
            continue
        if not symbol_lines:
            attempts.append(
                {
                    "tool_source": candidate.source,
                    "reason": "inspection produced no symbol lines",
                }
            )
            continue
        if missing_anchor_count:
            attempts.append(
                {
                    "tool_source": candidate.source,
                    "reason": "inspection missed required anchors",
                    "missing_anchor_count": missing_anchor_count,
                    "required_anchor_total": len(required_anchors),
                }
            )
            continue
        attempts.append(
            {
                "tool_source": candidate.source,
                "reason": "usable symbol table",
            }
        )
        prohibited_symbol_count = sum(last_prohibited_symbols.values())
        required_anchor_count = sum(
            count > 0 for count in required_anchors.values()
        )
        return {
            "tool_source": candidate.source,
            "status": completed.returncode,
            "line_count": len(symbol_lines),
            "prohibited_symbol_count": prohibited_symbol_count,
            "prohibited_symbols": last_prohibited_symbols,
            "required_anchor_count": required_anchor_count,
            "required_anchor_total": len(required_anchors),
            "required_anchors": required_anchors,
            "covers_product_seams": True,
            "attempts": attempts,
            "failure_reason": "",
        }

    prohibited_symbol_count = sum(last_prohibited_symbols.values())
    required_anchor_count = sum(
        count > 0 for count in last_required_anchors.values()
    )
    return {
        "tool_source": "",
        "status": last_status,
        "line_count": last_line_count,
        "prohibited_symbol_count": prohibited_symbol_count,
        "prohibited_symbols": last_prohibited_symbols,
        "required_anchor_count": required_anchor_count,
        "required_anchor_total": len(
            REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
        ),
        "required_anchors": last_required_anchors,
        "covers_product_seams": False,
        "attempts": attempts,
        "failure_reason": (
            "no usable archive-symbol inspection candidate"
        ),
    }


def relative_or_absolute(prefix: Path, path: Path) -> str:
    """@brief Render installed paths without assuming prefix containment.

    @param prefix Installation prefix preferred as the relative-path base.
    @param path Path to render.
    @return POSIX relative path when contained, otherwise absolute POSIX text.
    @throws None ``ValueError`` from containment checks is handled internally.
    @note The function is pure and keeps absolute install destinations diagnosable.
    """

    try:
        return path.relative_to(prefix).as_posix()
    except ValueError:
        return path.as_posix()


def exported_target_property(target_text: str, target: str, property_name: str) -> str:
    """@brief Extract one property from one exact exported target block.

    @param target_text Concatenated real ``Photospider*Targets*.cmake`` contents
      from every installed Photospider export set.
    @param target Exact imported target whose properties are authoritative.
    @param property_name Exact CMake property to extract.
    @return Unescaped quoted property value, or ``""`` if target/property is absent.
    @throws re.error Only if the fixed generated regular expressions are invalid.
    @note Only an exact target block can satisfy the match; unrelated targets
      and whole-file occurrences cannot. The function performs no CMake
      evaluation and has no file side effects.
    """

    target_pattern = re.compile(
        rf"set_target_properties\s*\(\s*{re.escape(target)}\s+PROPERTIES"
        rf"(?P<body>.*?)\n\s*\)",
        re.DOTALL,
    )
    property_pattern = re.compile(
        rf"\b{re.escape(property_name)}\s+\"(?P<value>(?:\\.|[^\"])*)\""
    )
    for target_match in target_pattern.finditer(target_text):
        property_match = property_pattern.search(target_match.group("body"))
        if property_match is not None:
            return property_match.group("value").replace("\\$", "$").replace('\\"', '"')
    return ""


def split_cmake_list(value: str) -> list[str]:
    """@brief Split a CMake list while preserving generator-expression lists.

    @param value CMake property value after quote and dollar unescaping.
    @return Nonempty top-level list entries in source order.
    @throws None The focused parser tolerates unmatched generator brackets by
      retaining the remaining text in the current item.
    @note Escaped semicolons remain literal; no variables or expressions execute.
    """

    entries: list[str] = []
    current: list[str] = []
    generator_depth = 0
    index = 0
    while index < len(value):
        if value[index] == "\\" and index + 1 < len(value):
            current.append(value[index + 1])
            index += 2
            continue
        if value.startswith("$<", index):
            generator_depth += 1
            current.extend(("$", "<"))
            index += 2
            continue
        if value[index] == ">" and generator_depth:
            generator_depth -= 1
            current.append(value[index])
            index += 1
            continue
        if value[index] == ";" and generator_depth == 0:
            entry = "".join(current)
            if entry:
                entries.append(entry)
            current = []
            index += 1
            continue
        current.append(value[index])
        index += 1
    final = "".join(current)
    if final:
        entries.append(final)
    return entries


def unwrap_link_only(entry: str) -> tuple[str, bool]:
    """@brief Classify one exported link-interface entry.

    @param entry One top-level ``INTERFACE_LINK_LIBRARIES`` item.
    @return Underlying payload and whether it uses ``$<LINK_ONLY:...>``.
    @throws None This is a pure exact-prefix/suffix matcher.
    @note Nested generator expressions remain inside the returned payload.
    """

    prefix = "$<LINK_ONLY:"
    if entry.startswith(prefix) and entry.endswith(">"):
        return entry[len(prefix) : -1], True
    return entry, False


def library_entry_matches(entry: str, library: str) -> bool:
    """@brief Match one accepted CMake target or exact library basename.

    @param entry Unwrapped exported link entry.
    @param library Canonical dependency name such as ``opencv_core`` or ``dl``.
    @return ``True`` for an explicit producer-accepted OpenCV target, an exact
      generic namespace target, or a platform library file.
    @throws None The function is a pure string match.
    @note OpenCV aliases form a closed component-specific list. Versioned
      shared/static suffixes are accepted only after an exact canonical
      basename, preventing partial-name false positives.
    """

    if entry in OPENCV_COMPONENT_TARGET_VARIANTS.get(library, ()):
        return True
    if entry == library or entry.endswith(f"::{library}"):
        return True
    basename = entry.replace("\\", "/").rsplit("/", 1)[-1]
    return bool(
        re.fullmatch(
            rf"(?:lib)?{re.escape(library)}(?:\.(?:a|lib|so(?:\.\d+)*|dylib))?",
            basename,
        )
    )


def classify_export_link_interface(
    entries: list[str], platform_system: str
) -> dict[str, Any]:
    """@brief Validate installed static-target dependency propagation.

    @param entries Parsed real ``INTERFACE_LINK_LIBRARIES`` entries.
    @param platform_system Build-host platform name from :mod:`platform`.
    @return Raw/unwrapped entries, required dependency observations, Apple flags,
      and aggregate link-only/platform contract booleans.
    @throws None All matching is deterministic and in-memory.
    @note Linux requires the expanded ``dl`` entry; Apple and Windows may expand
      ``CMAKE_DL_LIBS`` to an empty list. Framework flags are expected as plain
      propagated linker items on Apple, while library dependencies must be
      wrapped in ``LINK_ONLY``.
    """

    classified = [
        {
            "entry": entry,
            "value": unwrap_link_only(entry)[0],
            "link_only": unwrap_link_only(entry)[1],
        }
        for entry in entries
    ]

    def matching_rows(library: str) -> list[dict[str, Any]]:
        """@brief Select classified rows for one canonical library name.

        @param library Canonical target or library basename.
        @return Matching rows from the enclosing classified interface.
        @throws None Selection is pure and preserves source order.
        @note The nested helper captures no mutable state beyond ``classified``.
        """

        return [
            row for row in classified if library_entry_matches(row["value"], library)
        ]

    threads = matching_rows("Threads::Threads")
    yaml_cpp = matching_rows("yaml-cpp::yaml-cpp")
    opencv = {
        component: matching_rows(component) for component in REQUIRED_OPENCV_COMPONENTS
    }
    dl_rows = matching_rows("dl")
    required_rows = [*threads, *yaml_cpp]
    required_rows.extend(row for rows in opencv.values() for row in rows)
    required_rows.extend(dl_rows)
    required_present = bool(threads) and bool(yaml_cpp) and all(opencv.values())
    if platform_system == "Linux":
        required_present = required_present and bool(dl_rows)
    dependencies_are_link_only = required_present and all(
        row["link_only"] for row in required_rows
    )
    plain_entries = [row["value"] for row in classified if not row["link_only"]]
    if platform_system == "Darwin":
        apple_flags_correct = all(
            flag in plain_entries for flag in APPLE_PRODUCT_LINK_FLAGS
        )
    else:
        apple_flags_correct = all(
            flag not in plain_entries for flag in APPLE_PRODUCT_LINK_FLAGS
        )
    return {
        "classified_entries": classified,
        "link_only_entries": [row["value"] for row in classified if row["link_only"]],
        "plain_entries": plain_entries,
        "required_dependencies": {
            "threads": threads,
            "yaml_cpp": yaml_cpp,
            "opencv": opencv,
            "dl": dl_rows,
            "dl_required_for_platform": platform_system == "Linux",
        },
        "required_dependencies_are_link_only": dependencies_are_link_only,
        "apple_framework_flags_match_platform": apple_flags_correct,
    }


def find_consumer_executable(
    consumer_build: Path, requested_config: str, executable_name: str
) -> dict[str, Any]:
    """@brief Locate the built consumer using CMake target-file manifests.

    Manifest paths generated from ``$<TARGET_FILE:photospider_consumer>`` are
    authoritative for Xcode, Ninja Multi-Config, Visual Studio, and single-config
    generators. A filename fallback remains diagnostic protection for malformed
    or missing manifests.

    @param consumer_build Configured consumer build directory.
    @param requested_config Optional configuration passed to ``cmake --build``.
    @param executable_name Exact generated CMake executable target name.
    @return Manifest inventory, declared/fallback candidates, and selected file.
    @throws OSError If an existing manifest or build tree cannot be read.
    @note The function is read-only and accepts only regular existing files as
      runnable candidates. Fresh consumer builds prevent stale-config selection.
    """

    manifests = sorted(consumer_build.glob(f"{executable_name}_target_*.txt"))
    if requested_config:
        preferred_name = f"{executable_name}_target_{requested_config}.txt"
        manifests.sort(key=lambda path: path.name != preferred_name)
    declared_paths: list[Path] = []
    for manifest in manifests:
        value = manifest.read_text(encoding="utf-8").strip()
        if value:
            declared_paths.append(Path(value))

    fallback_paths = sorted(
        path
        for name in (executable_name, f"{executable_name}.exe")
        for path in consumer_build.rglob(name)
        if "CMakeFiles" not in path.parts
    )
    candidates: list[Path] = []
    for path in [*declared_paths, *fallback_paths]:
        if path not in candidates:
            candidates.append(path)
    selected = next((path for path in candidates if path.is_file()), None)
    return {
        "manifest_files": [path.as_posix() for path in manifests],
        "manifest_declared_paths": [path.as_posix() for path in declared_paths],
        "fallback_paths": [path.as_posix() for path in fallback_paths],
        "selected": selected.as_posix() if selected is not None else "",
        "selected_from_manifest": selected is not None and selected in declared_paths,
    }


def inspect_install_tree(
    repo: Path,
    prefix: Path,
    install_libdir: str,
    package_cmake_dir: str,
    platform_system: str,
) -> dict[str, Any]:
    """@brief Inspect installed files and all exported target-set metadata.

    @param repo Source repository used to detect leaked absolute paths.
    @param prefix Temporary installation prefix.
    @param install_libdir Configured library destination below ``prefix``.
    @param package_cmake_dir Relative or absolute package CMake destination
      containing the config file and every Photospider target export set.
    @param platform_system Host platform used for dl/framework expectations.
    @return Installed header/archive/config/export inventory, production archive
      symbol observations, and exact parsed target properties aggregated across
      all matching export-set files.
    @throws OSError If installed files cannot be traversed or read, or the
      selected symbol-inspection process cannot start.
    @note ``INTERFACE_LINK_LIBRARIES`` is derived from the real installed
      ``Photospider*Targets*.cmake`` files. File concatenation preserves each
      generated export fragment, while property parsing accepts only the exact
      requested target block.
    """

    headers = installed_headers(prefix)
    package_dir = Path(package_cmake_dir)
    if not package_dir.is_absolute():
        package_dir = prefix / package_dir
    targets_path = package_dir / "PhotospiderTargets.cmake"
    target_paths = sorted(package_dir.glob("Photospider*Targets*.cmake"))
    config_path = package_dir / "PhotospiderConfig.cmake"
    target_text = "\n".join(path.read_text(encoding="utf-8") for path in target_paths)
    config_text = (
        config_path.read_text(encoding="utf-8") if config_path.exists() else ""
    )
    unexpected_headers = [
        header for header in headers if not header.startswith("include/photospider/")
    ]
    source_root = repo.as_posix()
    archives = installed_static_product_archives(prefix, install_libdir)
    product_symbol_scan = inspect_product_archive_symbols(
        prefix, archives, platform_system
    )
    internal_test_product_artifacts = sorted(
        relative_or_absolute(prefix, path)
        for path in prefix.rglob("*")
        if path.is_file() and INTERNAL_TEST_PRODUCT_STEM in path.name.lower()
    )
    interface_link_raw = exported_target_property(
        target_text, EXPORTED_TARGET, "INTERFACE_LINK_LIBRARIES"
    )
    interface_link_entries = split_cmake_list(interface_link_raw)
    ipc_interface_link_raw = exported_target_property(
        target_text, IPC_EXPORTED_TARGET, "INTERFACE_LINK_LIBRARIES"
    )
    ipc_interface_link_entries = split_cmake_list(ipc_interface_link_raw)
    installed_ipc_headers = {
        header for header in headers if header.startswith("include/photospider/ipc/")
    }
    ipc_header_texts = {
        header: (prefix / header).read_text(encoding="utf-8")
        for header in sorted(installed_ipc_headers)
    }
    ipc_header_text = "\n".join(ipc_header_texts.values())
    ipc_include_directives = {
        header: re.findall(r"^\s*#\s*include\b[^\n]*", text, re.MULTILINE)
        for header, text in ipc_header_texts.items()
    }
    ipc_direct_includes = {
        header: re.findall(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", text, re.MULTILINE)
        for header, text in ipc_header_texts.items()
    }
    ipc_include_targets = [
        target
        for header in sorted(ipc_direct_includes)
        for target in ipc_direct_includes[header]
    ]
    unparsed_ipc_include_directives = sorted(
        directive
        for header, directives in ipc_include_directives.items()
        if len(directives) != len(ipc_direct_includes[header])
        for directive in directives
    )
    unexpected_ipc_includes = sorted(
        target
        for target in ipc_include_targets
        if target not in ALLOWED_IPC_STANDARD_INCLUDES
        and not (target.startswith("photospider/") and f"include/{target}" in headers)
    )
    forbidden_ipc_declarations = sorted(
        label
        for label, pattern in FORBIDDEN_IPC_DECLARATION_PATTERNS.items()
        if re.search(pattern, ipc_header_text, re.IGNORECASE)
    )
    exported_private_ipc_targets = sorted(
        part
        for part in FORBIDDEN_IPC_TARGET_PARTS[1:]
        if re.search(
            rf"add_library\s*\(\s*(?:Photospider::)?{re.escape(part)}\b",
            target_text,
        )
    )
    return {
        "headers": headers,
        "unexpected_headers": unexpected_headers,
        "static_product_archives": archives,
        "archive_exists": len(archives) == 1,
        "production_archive_symbol_scan": product_symbol_scan,
        "production_archive_omits_internal_test_symbols": (
            product_symbol_scan["status"] == 0
            and product_symbol_scan["line_count"] > 0
            and product_symbol_scan["covers_product_seams"]
            and product_symbol_scan["prohibited_symbol_count"] == 0
        ),
        "internal_test_product_artifacts": internal_test_product_artifacts,
        "install_omits_internal_test_product_artifacts": (
            internal_test_product_artifacts == []
        ),
        "config_exists": config_path.is_file(),
        "targets_exists": targets_path.is_file(),
        "target_files": [relative_or_absolute(prefix, path) for path in target_paths],
        "export_mentions_namespace_target": bool(
            re.search(
                rf"add_library\s*\(\s*{re.escape(EXPORTED_TARGET)}\s+STATIC\s+IMPORTED",
                target_text,
            )
        ),
        "export_mentions_ipc_namespace_target": bool(
            re.search(
                rf"add_library\s*\(\s*{re.escape(IPC_EXPORTED_TARGET)}\s+STATIC\s+IMPORTED",
                target_text,
            )
        ),
        "ipc_export_has_static_compile_definition": exported_target_property(
            target_text, IPC_EXPORTED_TARGET, "INTERFACE_COMPILE_DEFINITIONS"
        )
        == "PHOTOSPIDER_STATIC",
        "ipc_export_has_install_include_dir": "${_IMPORT_PREFIX}/include"
        in exported_target_property(
            target_text, IPC_EXPORTED_TARGET, "INTERFACE_INCLUDE_DIRECTORIES"
        ),
        "installed_ipc_headers": sorted(installed_ipc_headers),
        "ipc_install_headers_are_exact": (
            installed_ipc_headers == REQUIRED_IPC_PUBLIC_HEADERS
        ),
        "ipc_header_include_targets": ipc_include_targets,
        "ipc_header_direct_includes": ipc_direct_includes,
        "ipc_header_unexpected_includes": unexpected_ipc_includes,
        "ipc_header_unparsed_include_directives": unparsed_ipc_include_directives,
        "ipc_headers_use_exact_public_includes": (
            unexpected_ipc_includes == []
            and unparsed_ipc_include_directives == []
            and ipc_direct_includes
            == {
                header: list(includes)
                for header, includes in EXPECTED_IPC_DIRECT_INCLUDES.items()
            }
        ),
        "ipc_headers_forbidden_raw_declarations": forbidden_ipc_declarations,
        "ipc_headers_omit_raw_implementation_types": (forbidden_ipc_declarations == []),
        "ipc_export_link_interface": {
            "raw": ipc_interface_link_raw,
            "entries": ipc_interface_link_entries,
        },
        "ipc_export_links_only_threads": (
            ipc_interface_link_entries == ["Threads::Threads"]
        ),
        "ipc_export_omits_backend_dependency": all(
            part not in ipc_interface_link_raw for part in FORBIDDEN_IPC_TARGET_PARTS
        ),
        "exported_private_ipc_targets": exported_private_ipc_targets,
        "export_omits_private_ipc_targets": exported_private_ipc_targets == [],
        "ipc_client_archive_exists": any(
            path.name.lower()
            in {
                "libphotospider_ipc_client.a",
                "libphotospider_ipc_client.lib",
                "photospider_ipc_client.lib",
            }
            for path in (prefix / install_libdir).rglob("*")
            if path.is_file()
        ),
        "daemon_executable_exists": any(
            path.is_file()
            for path in (
                prefix / "bin" / "photospiderd",
                prefix / "bin" / "photospiderd.exe",
            )
        ),
        "export_has_static_compile_definition": exported_target_property(
            target_text, EXPORTED_TARGET, "INTERFACE_COMPILE_DEFINITIONS"
        )
        == "PHOTOSPIDER_STATIC",
        "export_has_install_include_dir": "${_IMPORT_PREFIX}/include"
        in exported_target_property(
            target_text, EXPORTED_TARGET, "INTERFACE_INCLUDE_DIRECTORIES"
        ),
        "export_omits_source_root": source_root not in target_text
        and source_root not in config_text,
        "export_omits_src_include_root": "/src" not in target_text,
        "export_omits_internal_test_product": (
            INTERNAL_TEST_PRODUCT_STEM not in target_text
            and all(
                definition not in target_text
                for definition in INTERNAL_PRODUCT_TEST_DEFINITIONS
            )
        ),
        "export_interface_link_libraries": {
            "raw": interface_link_raw,
            "entries": interface_link_entries,
            "classification": classify_export_link_interface(
                interface_link_entries, platform_system
            ),
        },
        "config_finds_opencv": "find_dependency(OpenCV" in config_text,
        "config_finds_yaml_cpp": "find_dependency(yaml-cpp)" in config_text,
        "config_finds_threads": "find_dependency(Threads)" in config_text,
        "config_defines_embedded_component": (
            "Photospider_embedded_FOUND TRUE" in config_text
        ),
        "config_defines_ipc_client_component": (
            "Photospider_ipc_client_FOUND ON" in config_text
        ),
    }


def archive_symbol_diagnostic_summary(
    symbol_scan: dict[str, Any],
) -> dict[str, Any]:
    """@brief Project one symbol scan onto its printable closed schema.

    @param symbol_scan In-memory archive observation used by package checks.
    @return Newly owned path-free diagnostic containing only controlled source,
      reason, symbol-token, status, and aggregate-count fields.
    @throws None Missing or malformed fields are replaced by safe defaults.
    @note The whitelist intentionally ignores unknown keys, executable/archive
      paths, raw symbol lines, captured streams, and environment data.
    """

    allowed_sources = {"xcrun llvm-nm", "PATH llvm-nm", "PATH nm"}
    allowed_reasons = {
        "xcrun is unavailable",
        "discovery could not start",
        "discovery exited with nonzero status",
        "discovery returned no path",
        "discovery returned multiple lines",
        "discovery returned a relative path",
        "discovery returned an unusable path",
        "executable is unavailable",
        "duplicate executable was skipped",
        "inspection could not start",
        "inspection exited with nonzero status",
        "inspection produced no symbol lines",
        "inspection missed required anchors",
        "usable symbol table",
    }
    allowed_failure_reasons = {
        "",
        "expected exactly one installed product archive",
        "no usable archive-symbol inspection candidate",
    }
    prohibited_input = symbol_scan.get("prohibited_symbols", {})
    if not isinstance(prohibited_input, dict):
        prohibited_input = {}
    prohibited_symbols: dict[str, int] = {}
    for symbol in FORBIDDEN_PRODUCT_TEST_SYMBOL_FRAGMENTS:
        count = prohibited_input.get(symbol, 0)
        if isinstance(count, int) and not isinstance(count, bool) and count > 0:
            prohibited_symbols[symbol] = count
    anchor_input = symbol_scan.get("required_anchors", {})
    if not isinstance(anchor_input, dict):
        anchor_input = {}
    required_anchors: dict[str, int] = {}
    for symbol in REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS:
        count = anchor_input.get(symbol, 0)
        required_anchors[symbol] = (
            count
            if isinstance(count, int)
            and not isinstance(count, bool)
            and count > 0
            else 0
        )
    attempts: list[dict[str, Any]] = []
    raw_attempts = symbol_scan.get("attempts", [])
    if not isinstance(raw_attempts, list):
        raw_attempts = []
    for raw_attempt in raw_attempts:
        if not isinstance(raw_attempt, dict):
            continue
        source = raw_attempt.get("tool_source", "")
        reason = raw_attempt.get("reason", "")
        if (
            not isinstance(source, str)
            or source not in allowed_sources
            or not isinstance(reason, str)
            or reason not in allowed_reasons
        ):
            continue
        attempt: dict[str, Any] = {
            "tool_source": source,
            "reason": reason,
        }
        status = raw_attempt.get("status")
        if isinstance(status, int) and not isinstance(status, bool):
            attempt["status"] = status
        missing_anchor_count = raw_attempt.get("missing_anchor_count")
        required_anchor_total = raw_attempt.get("required_anchor_total")
        if (
            isinstance(missing_anchor_count, int)
            and not isinstance(missing_anchor_count, bool)
            and missing_anchor_count >= 0
            and isinstance(required_anchor_total, int)
            and not isinstance(required_anchor_total, bool)
            and required_anchor_total >= 0
        ):
            attempt["missing_anchor_count"] = missing_anchor_count
            attempt["required_anchor_total"] = required_anchor_total
        attempts.append(attempt)
    tool_source = symbol_scan.get("tool_source", "")
    if not isinstance(tool_source, str) or tool_source not in allowed_sources:
        tool_source = ""
    status = symbol_scan.get("status")
    if not isinstance(status, int) or isinstance(status, bool):
        status = None
    line_count = symbol_scan.get("line_count", 0)
    if (
        not isinstance(line_count, int)
        or isinstance(line_count, bool)
        or line_count < 0
    ):
        line_count = 0
    failure_reason = symbol_scan.get("failure_reason", "")
    if (
        not isinstance(failure_reason, str)
        or failure_reason not in allowed_failure_reasons
    ):
        failure_reason = "no usable archive-symbol inspection candidate"
    return {
        "tool_source": tool_source,
        "status": status,
        "line_count": line_count,
        "prohibited_symbol_count": sum(prohibited_symbols.values()),
        "prohibited_symbols": prohibited_symbols,
        "required_anchor_count": sum(
            count > 0 for count in required_anchors.values()
        ),
        "required_anchor_total": len(
            REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
        ),
        "required_anchors": required_anchors,
        "covers_product_seams": (
            symbol_scan.get("covers_product_seams") is True
        ),
        "attempts": attempts,
        "failure_reason": failure_reason,
    }


def behavior_diagnostic_summary(
    observations: dict[str, Any], checks: dict[str, bool]
) -> dict[str, Any]:
    """@brief Build the only JSON projection allowed on package failure.

    @param observations Complete in-memory package observations used for verdicts.
    @param checks Stable named boolean behavior checks derived from observations.
    @return Path-free diagnostic with failed check labels, whitelisted command
      statuses, counts, and the sanitized archive-symbol observation.
    @throws KeyError If the required symbol-scan observation is absent.
    @note Full observations remain private because they contain transient build,
      install, workspace, executable, manifest, and package paths.
    """

    command_names = (
        "producer_configure",
        "build_photospider",
        "install",
        "embedded_consumer_configure",
        "embedded_consumer_build",
        "ipc_consumer_configure",
        "ipc_consumer_build",
        "policy_sdk_configure",
        "policy_sdk_build",
        "policy_contract_probe",
        "policy_cpp_contract_probe",
        "operation_sdk_configure",
        "operation_sdk_build",
        "operation_opencv_configure",
        "operation_opencv_build",
        "optional_opencv_missing_configure",
        "required_opencv_missing_configure",
        "unknown_component_configure",
        "consumer_run",
        "ipc_consumer_run",
        "operation_sdk_run",
        "operation_opencv_run",
    )
    command_input = observations.get("commands", {})
    if not isinstance(command_input, dict):
        command_input = {}
    command_statuses: dict[str, int | None | str] = {}
    for name in command_names:
        status = command_input.get(name)
        command_statuses[name] = (
            status
            if (status is None or isinstance(status, int))
            and not isinstance(status, bool)
            else "invalid status"
        )
    failed_checks = [
        name
        if re.fullmatch(r"[A-Za-z0-9 _+().,'-]+", name)
        else "unprintable behavior check"
        for name, passed in checks.items()
        if not passed
    ]
    symbol_scan = observations["install_tree"][
        "production_archive_symbol_scan"
    ]
    return {
        "failed_checks": failed_checks,
        "command_statuses": command_statuses,
        "archive_symbol_scan": archive_symbol_diagnostic_summary(symbol_scan),
    }


def emit_behavior_verdict(
    observations: dict[str, Any], checks: dict[str, bool]
) -> bool:
    """@brief Emit the CTest-facing package verdict and safe diagnostics.

    @param observations Complete in-memory package behavior observations.
    @param checks Stable named boolean behavior checks derived from observations.
    @return ``True`` only when every supplied check passes.
    @throws KeyError If the symbol-scan observation required for output is absent.
    @note Success and failure output share the sanitized symbol projection. On
      failure, only :func:`behavior_diagnostic_summary` is serialized; complete
      observations and captured tool data never reach stdout or stderr.
    """

    passed = all(checks.values())
    symbol_scan = archive_symbol_diagnostic_summary(
        observations["install_tree"]["production_archive_symbol_scan"]
    )
    print("static_product_consumer_smoke")
    print(
        "archive symbol scan: "
        f"source={symbol_scan['tool_source'] or 'none'}, "
        f"prohibited={symbol_scan['prohibited_symbol_count']}, "
        f"anchors={symbol_scan['required_anchor_count']}/"
        f"{symbol_scan['required_anchor_total']}"
    )
    for attempt in symbol_scan["attempts"]:
        details = [
            f"source={attempt['tool_source']}",
            f"reason={attempt['reason']}",
        ]
        if "status" in attempt:
            details.append(f"status={attempt['status']}")
        if "missing_anchor_count" in attempt:
            details.append(
                "missing_anchors="
                f"{attempt['missing_anchor_count']} of "
                f"{attempt['required_anchor_total']}"
            )
        print("archive symbol attempt: " + ", ".join(details))
    for name, ok in checks.items():
        print(f"{'PASS' if ok else 'FAIL'} {name}")
    if not passed:
        print(
            "package behavior diagnostic summary:\n"
            + json.dumps(
                behavior_diagnostic_summary(observations, checks),
                indent=2,
                sort_keys=True,
            ),
            file=sys.stderr,
        )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return passed


def evaluate_behavior(observations: dict[str, Any]) -> bool:
    """@brief Evaluate installed-package behavior in memory.

    @param observations Runtime observations from the producer, install tree,
      installed export sets, and external consumers.
    @return ``True`` only when every durable package invariant passes.
    @throws KeyError If the caller provides an incomplete observation schema.
    @note Results and a strict diagnostic projection are printed for CTest to
      capture; complete observations remain in memory and no report file is
      created. The projection excludes all transient paths and captured data.
    """

    install = observations["install_tree"]
    commands = observations["commands"]
    link_contract = install["export_interface_link_libraries"]["classification"]
    compiled_headers = sorted(
        "include/" + line.removeprefix("#include <").removesuffix(">")
        for line in observations["consumer"]["compiled_public_headers"]
    )
    checks = {
        "fresh producer configure succeeded when requested": (
            not observations["producer"]["configured_by_smoke"]
            or commands["producer_configure"] == 0
        ),
        "photospider target build succeeded": commands["build_photospider"] == 0,
        "install command succeeded": commands["install"] == 0,
        "installed static archive exists": install["archive_exists"],
        "installed product archive omits internal test symbols": install[
            "production_archive_omits_internal_test_symbols"
        ],
        "install omits internal test product artifacts": install[
            "install_omits_internal_test_product_artifacts"
        ],
        "package config and targets exist": install["config_exists"]
        and install["targets_exists"],
        "only include/photospider headers are installed": install["unexpected_headers"]
        == [],
        "installed public header inventory is exactly 21 files": len(install["headers"])
        == 21,
        "consumer compiles every installed public header": compiled_headers
        == install["headers"],
        "exported namespace target exists": install["export_mentions_namespace_target"],
        "exported IPC namespace target exists": install[
            "export_mentions_ipc_namespace_target"
        ],
        "exported IPC target carries PHOTOSPIDER_STATIC": install[
            "ipc_export_has_static_compile_definition"
        ],
        "exported IPC target uses install include root": install[
            "ipc_export_has_install_include_dir"
        ],
        "installed IPC public headers are exact": install[
            "ipc_install_headers_are_exact"
        ],
        "installed IPC headers use exact allowed public includes": install[
            "ipc_headers_use_exact_public_includes"
        ],
        "installed IPC headers omit raw implementation types": install[
            "ipc_headers_omit_raw_implementation_types"
        ],
        "exported IPC target links only Threads": install[
            "ipc_export_links_only_threads"
        ],
        "exported IPC target omits backend dependencies": install[
            "ipc_export_omits_backend_dependency"
        ],
        "package export omits private IPC targets": install[
            "export_omits_private_ipc_targets"
        ],
        "installed IPC client archive exists": install["ipc_client_archive_exists"],
        "installed photospiderd executable exists": install["daemon_executable_exists"],
        "exported target carries PHOTOSPIDER_STATIC": install[
            "export_has_static_compile_definition"
        ],
        "exported target uses install include root": install[
            "export_has_install_include_dir"
        ],
        "exported package omits source-tree include roots": install[
            "export_omits_source_root"
        ]
        and install["export_omits_src_include_root"],
        "export omits internal test product and definitions": install[
            "export_omits_internal_test_product"
        ],
        "installed export contains exact target link interface": bool(
            install["export_interface_link_libraries"]["raw"]
        ),
        "installed implementation dependencies are LINK_ONLY": link_contract[
            "required_dependencies_are_link_only"
        ],
        "installed Apple framework flags match platform": link_contract[
            "apple_framework_flags_match_platform"
        ],
        "package config finds dependencies": install["config_finds_opencv"]
        and install["config_finds_yaml_cpp"]
        and install["config_finds_threads"],
        "package config defines embedded and IPC components": install[
            "config_defines_embedded_component"
        ]
        and install["config_defines_ipc_client_component"],
        "embedded consumer configure succeeded": commands["embedded_consumer_configure"]
        == 0,
        "IPC-only component configure succeeds with backend discovery disabled": (
            commands["ipc_consumer_configure"] == 0
        ),
        "policy SDK configures with all backend discovery disabled": (
            commands["policy_sdk_configure"] == 0
        ),
        "policy SDK builds installed-header-only C11 and C++17 DSOs": (
            commands["policy_sdk_build"] == 0
        ),
        "installed policy DSO satisfies the exact ABI-v1 contract": (
            commands["policy_contract_probe"] == 0
        ),
        "installed C++17 policy DSO satisfies the exact ABI-v1 contract": (
            commands["policy_cpp_contract_probe"] == 0
        ),
        "operation SDK configures with all backend discovery disabled": (
            commands["operation_sdk_configure"] == 0
        ),
        "operation SDK builds a DSO and factory executable": (
            commands["operation_sdk_build"] == 0
        ),
        "operation OpenCV config requests only the core component": (
            commands["operation_opencv_configure"] == 0
        ),
        "operation OpenCV adapter builds with the core-only package shim": (
            commands["operation_opencv_build"] == 0
        ),
        "optional operation OpenCV stays absent when OpenCV is unavailable": (
            commands["optional_opencv_missing_configure"] == 0
        ),
        "required operation OpenCV fails when OpenCV is unavailable": (
            commands["required_opencv_missing_configure"] != 0
        ),
        "unknown required component configure fails": commands[
            "unknown_component_configure"
        ]
        != 0,
        "requested generator configured producer and both consumers": (
            not observations["producer"]["requested_generator"]
            or (
                observations["producer_cache"]["generator"]
                == observations["producer"]["requested_generator"]
                and observations["consumer"]["generator"]
                == observations["producer"]["requested_generator"]
                and observations["consumer"]["ipc_generator"]
                == observations["producer"]["requested_generator"]
            )
        ),
        "requested config belongs to each multi-config cache": (
            not observations["producer"]["requested_config"]
            or (
                not observations["producer_cache"]["configuration_types"]
                and not observations["consumer"]["configuration_types"]
                and not observations["consumer"]["ipc_configuration_types"]
            )
            or (
                observations["producer"]["requested_config"]
                in observations["producer_cache"]["configuration_types"].split(";")
                and observations["producer"]["requested_config"]
                in observations["consumer"]["configuration_types"].split(";")
                and observations["producer"]["requested_config"]
                in observations["consumer"]["ipc_configuration_types"].split(";")
            )
        ),
        "embedded consumer build succeeded": commands["embedded_consumer_build"] == 0,
        "IPC-only consumer build succeeded": commands["ipc_consumer_build"] == 0,
        "complete Client inventory is exact and unique": observations["consumer"][
            "surface_inventory"
        ]["client"]
        == list(EXPECTED_CLIENT_METHODS),
        "complete Host inventory is exact and unique": observations["consumer"][
            "surface_inventory"
        ]["host"]
        == list(EXPECTED_HOST_METHODS),
        "CMake target manifest resolves consumer executable": observations["consumer"][
            "executable_discovery"
        ]["selected_from_manifest"],
        "CMake target manifest resolves IPC consumer executable": observations[
            "consumer"
        ]["ipc_executable_discovery"]["selected_from_manifest"],
        "CMake target manifest resolves operation SDK executable": observations[
            "consumer"
        ]["operation_sdk_executable_discovery"]["selected_from_manifest"],
        "CMake target manifest resolves policy SDK plugin": observations["consumer"][
            "policy_plugin_discovery"
        ]["selected_from_manifest"],
        "CMake target manifest resolves C++17 policy SDK plugin": observations[
            "consumer"
        ]["cpp_policy_plugin_discovery"]["selected_from_manifest"],
        "CMake target manifest resolves operation SDK plugin": observations["consumer"][
            "operation_plugin_discovery"
        ]["selected_from_manifest"],
        "CMake target manifest resolves operation OpenCV executable": observations[
            "consumer"
        ]["operation_opencv_executable_discovery"]["selected_from_manifest"],
        "installed Host loads both extension DSOs and computes through them": (
            commands["consumer_run"] == 0
        ),
        "IPC-only consumer executable ran successfully": commands["ipc_consumer_run"]
        == 0,
        "operation SDK-only factory executable ran successfully": commands[
            "operation_sdk_run"
        ]
        == 0,
        "operation OpenCV core-only executable ran successfully": commands[
            "operation_opencv_run"
        ]
        == 0,
    }
    return emit_behavior_verdict(observations, checks)


def main() -> int:
    """@brief Optionally configure, then build/install/consume the package.

    @return Process status 0 when every package invariant passes, else 1.
    @throws OSError If commands cannot start or transient files are inaccessible.
    @throws RuntimeError If transient-path cleanup leaves a path behind.
    @throws ValueError If fresh-producer mode could delete the source tree or
      test work directory, or if the work path itself is destructive.
    @note The CTest-facing result covers package behavior only. Commands and
      assertions write directly to the streams captured by CTest; the work
      directory contains only normal producer/consumer build inputs and outputs.
      On Darwin, every child configure inherits the producer's meaningful
      ``CMAKE_OSX_ARCHITECTURES`` value as one argv element; other platforms
      receive no macOS-specific option.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--config", default="")
    parser.add_argument(
        "--generator",
        default="",
        help="CMake generator used for both producer and consumer configure",
    )
    parser.add_argument("--cmake-executable", default="cmake")
    parser.add_argument("--configure-fresh-producer", action="store_true")
    parser.add_argument(
        "--producer-build-testing", choices=("ON", "OFF"), default="OFF"
    )
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    build = Path(args.build).resolve()
    work = Path(args.work).resolve()
    if work == repo or work in repo.parents or work == build:
        raise ValueError(
            f"refusing destructive test work path: repo={repo}, build={build}, "
            f"work={work}"
        )
    if args.configure_fresh_producer and build in work.parents:
        raise ValueError(
            "fresh-producer work directory must be outside the build tree: "
            f"build={build}, work={work}"
        )
    strict_remove_tree(work)
    work.mkdir(parents=True, exist_ok=True)

    prefix = work / "install-prefix"
    embedded_consumer_src = work / "embedded-consumer-src"
    embedded_consumer_build = work / "embedded-consumer-build"
    ipc_consumer_src = work / "ipc-consumer-src"
    ipc_consumer_build = work / "ipc-consumer-build"
    policy_sdk_src = work / "policy-sdk-consumer-src"
    policy_sdk_build = work / "policy-sdk-consumer-build"
    operation_sdk_src = work / "operation-sdk-consumer-src"
    operation_sdk_build = work / "operation-sdk-consumer-build"
    operation_opencv_src = work / "operation-opencv-consumer-src"
    operation_opencv_build = work / "operation-opencv-consumer-build"
    optional_opencv_missing_src = work / "optional-opencv-missing-src"
    optional_opencv_missing_build = work / "optional-opencv-missing-build"
    required_opencv_missing_src = work / "required-opencv-missing-src"
    required_opencv_missing_build = work / "required-opencv-missing-build"
    unknown_component_src = work / "unknown-component-src"
    unknown_component_build = work / "unknown-component-build"
    compiled_public_headers, surface_inventory = write_consumer_projects(
        repo, embedded_consumer_src, ipc_consumer_src
    )
    unknown_component_src.mkdir(parents=True)
    (unknown_component_src / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                "project(photospider_unknown_component LANGUAGES CXX)",
                "find_package(Photospider CONFIG REQUIRED",
                "    COMPONENTS unknown_component)",
                "",
            ]
        ),
        encoding="utf-8",
    )

    producer_configure_code: int | None = None
    if args.configure_fresh_producer:
        producer_configure_code = configure_fresh_producer(
            args.cmake_executable,
            repo,
            build,
            args.config,
            args.generator,
            args.producer_build_testing,
        )
    else:
        print(
            "fresh producer configure not requested; existing build tree used",
            flush=True,
        )

    opencv_config_dir_value = cmake_cache_value(build, "OpenCV_DIR")
    if not opencv_config_dir_value or opencv_config_dir_value.endswith("-NOTFOUND"):
        raise RuntimeError("producer cache does not contain a usable OpenCV_DIR")
    write_extension_consumer_projects(
        policy_sdk_src,
        operation_sdk_src,
        operation_opencv_src,
        Path(opencv_config_dir_value),
    )
    write_missing_opencv_component_projects(
        optional_opencv_missing_src, required_opencv_missing_src
    )

    build_product_command = [
        args.cmake_executable,
        "--build",
        str(build),
        "--target",
        "photospider",
        "photospider_operation_runtime",
        "photospider_operation_opencv",
        "photospider_ipc_client",
        "photospiderd",
    ]
    if args.config:
        build_product_command.extend(["--config", args.config])
    build_product_code = run_command(build_product_command, repo)

    install_command = [
        args.cmake_executable,
        "--install",
        str(build),
        "--prefix",
        str(prefix),
    ]
    if args.config:
        install_command.extend(["--config", args.config])
    install_code = run_command(install_command, repo)

    embedded_configure_command = [
        args.cmake_executable,
        "-S",
        str(embedded_consumer_src),
        "-B",
        str(embedded_consumer_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
    ]
    ipc_configure_command = [
        args.cmake_executable,
        "-S",
        str(ipc_consumer_src),
        "-B",
        str(ipc_consumer_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE",
        "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE",
    ]
    policy_sdk_configure_command = [
        args.cmake_executable,
        "-S",
        str(policy_sdk_src),
        "-B",
        str(policy_sdk_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_Threads=TRUE",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE",
        "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE",
    ]
    operation_sdk_configure_command = [
        args.cmake_executable,
        "-S",
        str(operation_sdk_src),
        "-B",
        str(operation_sdk_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_Threads=TRUE",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE",
        "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE",
    ]
    operation_opencv_configure_command = [
        args.cmake_executable,
        "-S",
        str(operation_opencv_src),
        "-B",
        str(operation_opencv_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        f"-DOpenCV_DIR={operation_opencv_src / 'opencv-core-shim'}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_Threads=TRUE",
        "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE",
    ]
    optional_opencv_missing_configure_command = [
        args.cmake_executable,
        "-S",
        str(optional_opencv_missing_src),
        "-B",
        str(optional_opencv_missing_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE",
    ]
    required_opencv_missing_configure_command = [
        args.cmake_executable,
        "-S",
        str(required_opencv_missing_src),
        "-B",
        str(required_opencv_missing_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE",
    ]
    unknown_component_configure_command = [
        args.cmake_executable,
        "-S",
        str(unknown_component_src),
        "-B",
        str(unknown_component_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
    ]
    if args.generator:
        for command in (
            embedded_configure_command,
            ipc_configure_command,
            policy_sdk_configure_command,
            operation_sdk_configure_command,
            operation_opencv_configure_command,
            optional_opencv_missing_configure_command,
            required_opencv_missing_configure_command,
            unknown_component_configure_command,
        ):
            command.extend(["-G", args.generator])
    osx_architecture_arguments = producer_osx_architecture_arguments(build)
    osx_architectures = (
        osx_architecture_arguments[0].split("=", 1)[1]
        if osx_architecture_arguments
        else ""
    )
    install_libdir = cmake_cache_value(build, "CMAKE_INSTALL_LIBDIR") or "lib"
    package_cmake_dir = (
        cmake_cache_value(build, "PHOTOSPIDER_INSTALL_CMAKEDIR")
        or f"{install_libdir}/cmake/Photospider"
    )
    for command in (
        embedded_configure_command,
        ipc_configure_command,
        policy_sdk_configure_command,
        operation_sdk_configure_command,
        operation_opencv_configure_command,
        optional_opencv_missing_configure_command,
        required_opencv_missing_configure_command,
        unknown_component_configure_command,
    ):
        command.extend(osx_architecture_arguments)
    embedded_configure_code = run_command(embedded_configure_command, repo)
    ipc_configure_code = run_command(ipc_configure_command, repo)
    policy_sdk_configure_code = run_command(policy_sdk_configure_command, repo)
    operation_sdk_configure_code = run_command(operation_sdk_configure_command, repo)
    operation_opencv_configure_code = run_command(
        operation_opencv_configure_command, repo
    )
    optional_opencv_missing_configure_code = run_command(
        optional_opencv_missing_configure_command, repo
    )
    required_opencv_missing_configure_code = run_command(
        required_opencv_missing_configure_command, repo
    )
    unknown_component_configure_code = run_command(
        unknown_component_configure_command, repo
    )

    embedded_build_command = [
        args.cmake_executable,
        "--build",
        str(embedded_consumer_build),
    ]
    ipc_build_command = [
        args.cmake_executable,
        "--build",
        str(ipc_consumer_build),
    ]
    policy_sdk_build_command = [
        args.cmake_executable,
        "--build",
        str(policy_sdk_build),
    ]
    operation_sdk_build_command = [
        args.cmake_executable,
        "--build",
        str(operation_sdk_build),
    ]
    operation_opencv_build_command = [
        args.cmake_executable,
        "--build",
        str(operation_opencv_build),
    ]
    if args.config:
        embedded_build_command.extend(["--config", args.config])
        ipc_build_command.extend(["--config", args.config])
        policy_sdk_build_command.extend(["--config", args.config])
        operation_sdk_build_command.extend(["--config", args.config])
        operation_opencv_build_command.extend(["--config", args.config])
    embedded_build_code = run_command(embedded_build_command, repo)
    ipc_build_code = run_command(ipc_build_command, repo)
    policy_sdk_build_code = run_command(policy_sdk_build_command, repo)
    operation_sdk_build_code = run_command(operation_sdk_build_command, repo)
    operation_opencv_build_code = run_command(operation_opencv_build_command, repo)

    executable_discovery = find_consumer_executable(
        embedded_consumer_build, args.config, "photospider_consumer"
    )
    ipc_executable_discovery = find_consumer_executable(
        ipc_consumer_build, args.config, "photospider_ipc_consumer"
    )
    operation_sdk_executable_discovery = find_consumer_executable(
        operation_sdk_build, args.config, "installed_operation_factory"
    )
    policy_plugin_discovery = find_consumer_executable(
        policy_sdk_build, args.config, "installed_policy_plugin"
    )
    cpp_policy_plugin_discovery = find_consumer_executable(
        policy_sdk_build, args.config, "installed_cpp_policy_plugin"
    )
    operation_plugin_discovery = find_consumer_executable(
        operation_sdk_build, args.config, "installed_operation_plugin"
    )
    operation_opencv_executable_discovery = find_consumer_executable(
        operation_opencv_build, args.config, "installed_operation_opencv"
    )
    consumer_generator = cmake_cache_value(embedded_consumer_build, "CMAKE_GENERATOR")
    consumer_configuration_types = cmake_cache_value(
        embedded_consumer_build, "CMAKE_CONFIGURATION_TYPES"
    )
    ipc_consumer_generator = cmake_cache_value(ipc_consumer_build, "CMAKE_GENERATOR")
    ipc_consumer_configuration_types = cmake_cache_value(
        ipc_consumer_build, "CMAKE_CONFIGURATION_TYPES"
    )
    executable = (
        Path(executable_discovery["selected"])
        if executable_discovery["selected"]
        else None
    )
    policy_plugin = (
        Path(policy_plugin_discovery["selected"])
        if policy_plugin_discovery["selected"]
        else None
    )
    cpp_policy_plugin = (
        Path(cpp_policy_plugin_discovery["selected"])
        if cpp_policy_plugin_discovery["selected"]
        else None
    )
    operation_plugin = (
        Path(operation_plugin_discovery["selected"])
        if operation_plugin_discovery["selected"]
        else None
    )
    policy_contract_probe_code = 127
    if policy_plugin is not None and policy_plugin.is_file():
        policy_contract_probe_code = run_installed_policy_contract_probe(
            policy_plugin
        )
    else:
        print(
            "policy SDK plugin not found for contract probe; discovery="
            + json.dumps(policy_plugin_discovery, sort_keys=True),
            file=sys.stderr,
            flush=True,
        )
    cpp_policy_contract_probe_code = 127
    if cpp_policy_plugin is not None and cpp_policy_plugin.is_file():
        cpp_policy_contract_probe_code = run_installed_policy_contract_probe(
            cpp_policy_plugin
        )
    else:
        print(
            "C++17 policy SDK plugin not found for contract probe; discovery="
            + json.dumps(cpp_policy_plugin_discovery, sort_keys=True),
            file=sys.stderr,
            flush=True,
        )
    extension_graph = work / "installed-extension-graph.yaml"
    extension_graph.write_text(
        "\n".join(
            [
                "- id: 1",
                "  name: installed_factory",
                "  type: installed",
                "  subtype: factory",
                "",
            ]
        ),
        encoding="utf-8",
    )
    run_code = 127
    if (
        executable is not None
        and executable.is_file()
        and cpp_policy_plugin is not None
        and cpp_policy_plugin.is_file()
        and operation_plugin is not None
        and operation_plugin.is_file()
    ):
        run_code = run_command(
            [
                str(executable),
                str(cpp_policy_plugin),
                str(operation_plugin),
                str(work / "installed-extension-sessions"),
                str(extension_graph),
            ],
            repo,
        )
    else:
        print(
            "consumer or extension plugin not found; discovery="
            + json.dumps(
                {
                    "consumer": executable_discovery,
                    "policy": policy_plugin_discovery,
                    "cpp_policy": cpp_policy_plugin_discovery,
                    "operation": operation_plugin_discovery,
                },
                sort_keys=True,
            ),
            file=sys.stderr,
            flush=True,
        )
    ipc_executable = (
        Path(ipc_executable_discovery["selected"])
        if ipc_executable_discovery["selected"]
        else None
    )
    ipc_run_code = 127
    if ipc_executable is not None and ipc_executable.is_file():
        ipc_run_code = run_command([str(ipc_executable)], repo)
    else:
        print(
            "IPC consumer executable not found; discovery="
            + json.dumps(ipc_executable_discovery, sort_keys=True),
            file=sys.stderr,
            flush=True,
        )

    operation_sdk_executable = (
        Path(operation_sdk_executable_discovery["selected"])
        if operation_sdk_executable_discovery["selected"]
        else None
    )
    operation_sdk_run_code = 127
    if operation_sdk_executable is not None and operation_sdk_executable.is_file():
        operation_sdk_run_code = run_command([str(operation_sdk_executable)], repo)
    else:
        print(
            "operation SDK consumer executable not found; discovery="
            + json.dumps(operation_sdk_executable_discovery, sort_keys=True),
            file=sys.stderr,
            flush=True,
        )

    operation_opencv_executable = (
        Path(operation_opencv_executable_discovery["selected"])
        if operation_opencv_executable_discovery["selected"]
        else None
    )
    operation_opencv_run_code = 127
    if (
        operation_opencv_executable is not None
        and operation_opencv_executable.is_file()
    ):
        operation_opencv_run_code = run_command(
            [str(operation_opencv_executable)], repo
        )
    else:
        print(
            "operation OpenCV consumer executable not found; discovery="
            + json.dumps(operation_opencv_executable_discovery, sort_keys=True),
            file=sys.stderr,
            flush=True,
        )

    platform_system = platform.system()
    producer_build_testing = cmake_cache_value(build, "BUILD_TESTING")
    install_tree = inspect_install_tree(
        repo,
        prefix,
        install_libdir,
        package_cmake_dir,
        platform_system,
    )
    observations = {
        "commands": {
            "producer_configure": producer_configure_code,
            "build_photospider": build_product_code,
            "install": install_code,
            "embedded_consumer_configure": embedded_configure_code,
            "embedded_consumer_build": embedded_build_code,
            "ipc_consumer_configure": ipc_configure_code,
            "ipc_consumer_build": ipc_build_code,
            "policy_sdk_configure": policy_sdk_configure_code,
            "policy_sdk_build": policy_sdk_build_code,
            "policy_contract_probe": policy_contract_probe_code,
            "policy_cpp_contract_probe": cpp_policy_contract_probe_code,
            "operation_sdk_configure": operation_sdk_configure_code,
            "operation_sdk_build": operation_sdk_build_code,
            "operation_opencv_configure": operation_opencv_configure_code,
            "operation_opencv_build": operation_opencv_build_code,
            "optional_opencv_missing_configure": (
                optional_opencv_missing_configure_code
            ),
            "required_opencv_missing_configure": (
                required_opencv_missing_configure_code
            ),
            "unknown_component_configure": unknown_component_configure_code,
            "consumer_run": run_code,
            "ipc_consumer_run": ipc_run_code,
            "operation_sdk_run": operation_sdk_run_code,
            "operation_opencv_run": operation_opencv_run_code,
        },
        "install_tree": install_tree,
        "paths": {
            "cmake_executable": args.cmake_executable,
            "install_libdir": install_libdir,
            "package_cmake_dir": package_cmake_dir,
            "consumer_osx_architectures": osx_architectures,
            "platform_system": platform_system,
        },
        "consumer": {
            "compiled_public_headers": compiled_public_headers,
            "generator": consumer_generator,
            "configuration_types": consumer_configuration_types,
            "ipc_generator": ipc_consumer_generator,
            "ipc_configuration_types": ipc_consumer_configuration_types,
            "executable_discovery": executable_discovery,
            "ipc_executable_discovery": ipc_executable_discovery,
            "operation_sdk_executable_discovery": (operation_sdk_executable_discovery),
            "policy_plugin_discovery": policy_plugin_discovery,
            "cpp_policy_plugin_discovery": cpp_policy_plugin_discovery,
            "operation_plugin_discovery": operation_plugin_discovery,
            "operation_opencv_executable_discovery": (
                operation_opencv_executable_discovery
            ),
            "surface_inventory": surface_inventory,
        },
        "producer": {
            "configured_by_smoke": args.configure_fresh_producer,
            "build_testing": producer_build_testing,
            "requested_config": args.config,
            "requested_generator": args.generator,
        },
        "producer_cache": {
            "generator": cmake_cache_value(build, "CMAKE_GENERATOR"),
            "configuration_types": cmake_cache_value(
                build, "CMAKE_CONFIGURATION_TYPES"
            ),
            "build_type": cmake_cache_value(build, "CMAKE_BUILD_TYPE"),
            "build_testing": producer_build_testing,
        },
    }
    passed = evaluate_behavior(observations)
    strict_remove_tree(work)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
