#!/usr/bin/env python3
"""Install Photospider and verify an external CMake consumer can link it."""

from __future__ import annotations

import argparse
import json
import platform
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


STATIC_PRODUCT_ARCHIVE_NAMES = {
    "libphotospider.a",
    "libphotospider.lib",
    "photospider.lib",
}
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
        r"PluginManager|SchedulerPluginLoader)\b"
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
    "scheduler_available_types",
    "scheduler_description",
    "scheduler_scan",
    "scheduler_load",
    "scheduler_loaded_plugins",
    "configure_scheduler_defaults",
    "scheduler_info",
    "replace_scheduler",
    "scheduler_trace",
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
    "scheduler_trace",
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
    "scheduler_available_types",
    "scheduler_description",
    "scheduler_scan",
    "scheduler_load",
    "scheduler_loaded_plugins",
    "configure_scheduler_defaults",
    "scheduler_info",
    "replace_scheduler",
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
APPLE_PRODUCT_LINK_FLAGS = ("-framework Metal", "-framework Foundation")


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
#include <utility>
#include <vector>

namespace {

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
  const ps::HostSchedulerConfig scheduler_config{};
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
  (void)client.scheduler_available_types();
  (void)client.scheduler_description(text);
  (void)client.scheduler_scan(paths);
  (void)client.scheduler_load(text);
  (void)client.scheduler_loaded_plugins();
  (void)client.configure_scheduler_defaults(scheduler_config);
  (void)client.scheduler_info(ipc_session, intent);
  (void)client.replace_scheduler(ipc_session, intent, text);
  (void)client.scheduler_trace(ipc_session, 0, 1);

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
  (void)host.scheduler_trace(host_session, 0, 1);
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
  (void)host.scheduler_available_types();
  (void)host.scheduler_description(text);
  (void)host.scheduler_scan(paths);
  (void)host.scheduler_load(text);
  (void)host.scheduler_loaded_plugins();
  (void)host.configure_scheduler_defaults(scheduler_config);
  (void)host.scheduler_info(host_session, intent);
  (void)host.replace_scheduler(host_session, intent, text);
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
      exactly 55 unique Client and 53 unique Host names.
    @note This validates maintained product symbols rather than source-migration
      residue and remains part of the existing package-consumer behavior test.
    """

    expected = {
        "client": (55, EXPECTED_CLIENT_METHODS),
        "host": (53, EXPECTED_HOST_METHODS),
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

    The embedded project uses default package semantics and compiles every
    installed public header. The IPC project explicitly requests only the
    ``ipc_client`` component and links only its exported target. Each project
    generates its own target manifest for single- and multi-config generators.

    @param repo Repository root used only to inventory public headers.
    @param embedded_source_dir Transient embedded consumer source directory.
    @param ipc_source_dir Transient IPC-only consumer source directory.
    @return Embedded include directives and verified IPC call inventories.
    @throws OSError If directories or generated source files cannot be written.
    @throws RuntimeError If the IPC harness does not reference the exact 55
      Client and 53 Host operation names once each.
    @note :func:`main` recreates the surrounding work directory on every run.
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
                "    PRIVATE Photospider::photospider)",
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
                "#include <cstddef>",
                "#include <memory>",
                "",
                *include_lines,
                "",
                "int main() {",
                "  auto host = ps::create_embedded_host();",
                "  if (!host) {",
                "    return 1;",
                "  }",
                "  const std::size_t step = ps::aligned_image_buffer_step(",
                "      8, 4, ps::DataType::FLOAT32);",
                "  auto image = ps::make_aligned_cpu_image_buffer(",
                "      2, 2, 4, ps::DataType::FLOAT32);",
                "  return step == 128 && image.data ? 0 : 2;",
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

    @param target_text Concatenated real ``PhotospiderTargets*.cmake`` contents.
    @param target Exact imported target whose properties are authoritative.
    @param property_name Exact CMake property to extract.
    @return Unescaped quoted property value, or ``""`` if target/property is absent.
    @throws re.error Only if the fixed generated regular expressions are invalid.
    @note Unrelated targets and whole-file occurrences cannot satisfy the match;
      the function performs no CMake evaluation and has no file side effects.
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
    """@brief Match a CMake target, linker name, or library path by basename.

    @param entry Unwrapped exported link entry.
    @param library Canonical dependency name such as ``opencv_core`` or ``dl``.
    @return ``True`` for exact/namespace target names or platform library files.
    @throws None The function is a pure string match.
    @note Versioned shared/static suffixes are accepted only after an exact
      canonical basename, preventing partial-name false positives.
    """

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
    """@brief Inspect installed files and the exact exported target metadata.

    @param repo Source repository used to detect leaked absolute paths.
    @param prefix Temporary installation prefix.
    @param install_libdir Configured library destination below ``prefix``.
    @param package_cmake_dir Relative or absolute package CMake destination.
    @param platform_system Host platform used for dl/framework expectations.
    @return Installed header/archive/config inventory and parsed target properties.
    @throws OSError If installed files cannot be traversed or read.
    @note ``INTERFACE_LINK_LIBRARIES`` is derived from the real installed
      ``PhotospiderTargets*.cmake`` files and only the exact exported target block.
    """

    headers = installed_headers(prefix)
    package_dir = Path(package_cmake_dir)
    if not package_dir.is_absolute():
        package_dir = prefix / package_dir
    targets_path = package_dir / "PhotospiderTargets.cmake"
    target_paths = sorted(package_dir.glob("PhotospiderTargets*.cmake"))
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


def evaluate_behavior(observations: dict[str, Any]) -> bool:
    """@brief Evaluate installed-package behavior in memory.

    @param observations Runtime observations from the producer, install tree,
      exported target, and external consumer.
    @return ``True`` only when every durable package invariant passes.
    @throws KeyError If the caller provides an incomplete observation schema.
    @note Results and failure observations are printed directly for CTest to
      capture; this function creates no report or comparison files.
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
        "package config and targets exist": install["config_exists"]
        and install["targets_exists"],
        "only include/photospider headers are installed": install["unexpected_headers"]
        == [],
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
        "consumer executable ran successfully": commands["consumer_run"] == 0,
        "IPC-only consumer executable ran successfully": commands["ipc_consumer_run"]
        == 0,
    }
    passed = all(checks.values())
    print("static_product_consumer_smoke")
    for name, ok in checks.items():
        print(f"{'PASS' if ok else 'FAIL'} {name}")
    if install["unexpected_headers"]:
        print("unexpected installed headers:", file=sys.stderr)
        for header in install["unexpected_headers"]:
            print(f"- {header}", file=sys.stderr)
    if not passed:
        print(
            "package behavior observations:\n"
            + json.dumps(observations, indent=2, sort_keys=True),
            file=sys.stderr,
        )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return passed


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

    build_product_command = [
        args.cmake_executable,
        "--build",
        str(build),
        "--target",
        "photospider",
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
            unknown_component_configure_command,
        ):
            command.extend(["-G", args.generator])
    osx_architectures = cmake_cache_value(build, "CMAKE_OSX_ARCHITECTURES")
    install_libdir = cmake_cache_value(build, "CMAKE_INSTALL_LIBDIR") or "lib"
    package_cmake_dir = (
        cmake_cache_value(build, "PHOTOSPIDER_INSTALL_CMAKEDIR")
        or f"{install_libdir}/cmake/Photospider"
    )
    if osx_architectures:
        for command in (embedded_configure_command, ipc_configure_command):
            command.append(f"-DCMAKE_OSX_ARCHITECTURES={osx_architectures}")
    embedded_configure_code = run_command(embedded_configure_command, repo)
    ipc_configure_code = run_command(ipc_configure_command, repo)
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
    if args.config:
        embedded_build_command.extend(["--config", args.config])
        ipc_build_command.extend(["--config", args.config])
    embedded_build_code = run_command(embedded_build_command, repo)
    ipc_build_code = run_command(ipc_build_command, repo)

    executable_discovery = find_consumer_executable(
        embedded_consumer_build, args.config, "photospider_consumer"
    )
    ipc_executable_discovery = find_consumer_executable(
        ipc_consumer_build, args.config, "photospider_ipc_consumer"
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
    run_code = 127
    if executable is not None and executable.is_file():
        run_code = run_command([str(executable)], repo)
    else:
        print(
            "consumer executable not found; discovery="
            + json.dumps(executable_discovery, sort_keys=True),
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
            "unknown_component_configure": unknown_component_configure_code,
            "consumer_run": run_code,
            "ipc_consumer_run": ipc_run_code,
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
