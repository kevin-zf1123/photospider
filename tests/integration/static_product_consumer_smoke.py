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
            f"command failed with exit code {proc.returncode}: "
            + shlex.join(command),
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


def write_consumer_project(repo: Path, source_dir: Path) -> list[str]:
    """@brief Create an isolated installed-package consumer project.

    The generated CMake project links the exported target, compiles every public
    header, and uses ``file(GENERATE)`` with ``$<TARGET_FILE:...>`` so the test can
    locate executables for single- and multi-configuration generators.

    @param repo Repository root used only to inventory public headers.
    @param source_dir Transient consumer source directory to create or update.
    @return Include directives compiled by the generated ``main.cpp``.
    @throws OSError If directories or generated source files cannot be written.
    @note Replaces ``CMakeLists.txt`` and ``main.cpp`` under ``source_dir`` only;
      :func:`main` recreates the surrounding work directory on each invocation.
    """

    source_dir.mkdir(parents=True, exist_ok=True)
    include_lines = public_header_includes(repo)
    (source_dir / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                "project(photospider_consumer_smoke LANGUAGES CXX)",
                "find_package(Photospider CONFIG REQUIRED)",
                "add_executable(photospider_consumer main.cpp)",
                "target_link_libraries(photospider_consumer",
                "    PRIVATE Photospider::photospider)",
                "add_executable(photospider_ipc_consumer ipc_main.cpp)",
                "target_link_libraries(photospider_ipc_consumer",
                "    PRIVATE Photospider::photospider_ipc_client)",
                "file(GENERATE",
                '    OUTPUT "${CMAKE_BINARY_DIR}/photospider_consumer_target_$<CONFIG>.txt"',
                '    CONTENT "$<TARGET_FILE:photospider_consumer>\\n")',
                "file(GENERATE",
                '    OUTPUT "${CMAKE_BINARY_DIR}/photospider_ipc_consumer_target_$<CONFIG>.txt"',
                '    CONTENT "$<TARGET_FILE:photospider_ipc_consumer>\\n")',
                "",
            ]
        ),
        encoding="utf-8",
    )
    (source_dir / "main.cpp").write_text(
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
    (source_dir / "ipc_main.cpp").write_text(
        "\n".join(
            [
                "#include <photospider/ipc/client.hpp>",
                "#include <photospider/ipc/protocol.hpp>",
                "",
                "int main() {",
                "  ps::ipc::Client client;",
                "  const ps::ipc::IpcStatus status = client.connect(\"\");",
                "  client.disconnect();",
                "  return !status.ok &&",
                "                 status.domain == ps::ipc::IpcErrorDomain::Transport",
                "             ? 0",
                "             : 1;",
                "}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return include_lines


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

    manifests = sorted(
        consumer_build.glob(f"{executable_name}_target_*.txt")
    )
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
        "ipc_export_omits_internal_dependencies": (
            "nlohmann_json" not in ipc_interface_link_raw
            and "photospider_ipc_server_internal" not in ipc_interface_link_raw
            and "photospider_ipc_client_objects" not in ipc_interface_link_raw
            and "Photospider::photospider" not in ipc_interface_link_raw
        ),
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
        "exported IPC target omits internal dependencies": install[
            "ipc_export_omits_internal_dependencies"
        ],
        "installed IPC client archive exists": install[
            "ipc_client_archive_exists"
        ],
        "installed photospiderd executable exists": install[
            "daemon_executable_exists"
        ],
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
        "consumer configure succeeded": commands["consumer_configure"] == 0,
        "requested generator configured producer and consumer": (
            not observations["producer"]["requested_generator"]
            or (
                observations["producer_cache"]["generator"]
                == observations["producer"]["requested_generator"]
                and observations["consumer"]["generator"]
                == observations["producer"]["requested_generator"]
            )
        ),
        "requested config belongs to each multi-config cache": (
            not observations["producer"]["requested_config"]
            or (
                not observations["producer_cache"]["configuration_types"]
                and not observations["consumer"]["configuration_types"]
            )
            or (
                observations["producer"]["requested_config"]
                in observations["producer_cache"]["configuration_types"].split(";")
                and observations["producer"]["requested_config"]
                in observations["consumer"]["configuration_types"].split(";")
            )
        ),
        "consumer build succeeded": commands["consumer_build"] == 0,
        "CMake target manifest resolves consumer executable": observations[
            "consumer"
        ]["executable_discovery"]["selected_from_manifest"],
        "CMake target manifest resolves IPC consumer executable": observations[
            "consumer"
        ]["ipc_executable_discovery"]["selected_from_manifest"],
        "consumer executable ran successfully": commands["consumer_run"] == 0,
        "IPC-only consumer executable ran successfully": commands[
            "ipc_consumer_run"
        ]
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
    consumer_src = work / "consumer-src"
    consumer_build = work / "consumer-build"
    compiled_public_headers = write_consumer_project(repo, consumer_src)

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

    configure_command = [
        args.cmake_executable,
        "-S",
        str(consumer_src),
        "-B",
        str(consumer_build),
        f"-DCMAKE_PREFIX_PATH={prefix}",
    ]
    if args.generator:
        configure_command.extend(["-G", args.generator])
    osx_architectures = cmake_cache_value(build, "CMAKE_OSX_ARCHITECTURES")
    install_libdir = cmake_cache_value(build, "CMAKE_INSTALL_LIBDIR") or "lib"
    package_cmake_dir = (
        cmake_cache_value(build, "PHOTOSPIDER_INSTALL_CMAKEDIR")
        or f"{install_libdir}/cmake/Photospider"
    )
    if osx_architectures:
        configure_command.append(f"-DCMAKE_OSX_ARCHITECTURES={osx_architectures}")
    configure_code = run_command(configure_command, repo)

    build_command = [args.cmake_executable, "--build", str(consumer_build)]
    if args.config:
        build_command.extend(["--config", args.config])
    build_code = run_command(build_command, repo)

    executable_discovery = find_consumer_executable(
        consumer_build, args.config, "photospider_consumer"
    )
    ipc_executable_discovery = find_consumer_executable(
        consumer_build, args.config, "photospider_ipc_consumer"
    )
    consumer_generator = cmake_cache_value(consumer_build, "CMAKE_GENERATOR")
    consumer_configuration_types = cmake_cache_value(
        consumer_build, "CMAKE_CONFIGURATION_TYPES"
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
            "consumer_configure": configure_code,
            "consumer_build": build_code,
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
            "executable_discovery": executable_discovery,
            "ipc_executable_discovery": ipc_executable_discovery,
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
