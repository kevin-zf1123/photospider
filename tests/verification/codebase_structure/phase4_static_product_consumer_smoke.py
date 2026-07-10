#!/usr/bin/env python3
"""Install Photospider and verify an external CMake consumer can link it."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable
from unittest.mock import Mock


STATIC_PRODUCT_ARCHIVE_NAMES = {
    "libphotospider.a",
    "libphotospider.lib",
    "photospider.lib",
}
EXPORTED_TARGET = "Photospider::photospider"
TARGET_MANIFEST_GLOB = "photospider_consumer_target_*.txt"
REQUIRED_OPENCV_COMPONENTS = (
    "opencv_core",
    "opencv_imgproc",
    "opencv_imgcodecs",
    "opencv_videoio",
)
APPLE_PRODUCT_LINK_FLAGS = ("-framework Metal", "-framework Foundation")


def strict_remove_tree(
    path: Path, remover: Callable[[Path], None] = shutil.rmtree
) -> dict[str, Any]:
    """@brief Remove one freshness-critical directory without hiding failure.

    The helper snapshots the real path and cache state, invokes the supplied
    recursive remover only when the path exists (including a final symlink),
    and then verifies that neither the path nor a nested ``CMakeCache.txt``
    remains. A cleanup that returns without removing the path is therefore as
    fatal as an exception raised by the filesystem operation.

    @param path Directory whose previous contents must not survive this run.
    @param remover Recursive directory remover; injection is reserved for the
      executable synthetic failure/residual contract.
    @return Filesystem-derived before/after/cache observations and a derived
      ``freshness_verified`` result.
    @throws OSError If filesystem inspection or recursive removal fails.
    @throws RuntimeError If ``remover`` returns while ``path`` or its cache
      still exists.
    @note The function never suppresses cleanup errors and never creates
      ``path``. Callers must independently validate that destructive paths are
      outside protected source/evidence roots before invoking it.
    """

    existed_before = path.exists() or path.is_symlink()
    cache_path = path / "CMakeCache.txt"
    cache_existed_before = cache_path.exists() or cache_path.is_symlink()
    if existed_before:
        remover(path)
    exists_after = path.exists() or path.is_symlink()
    cache_exists_after = cache_path.exists() or cache_path.is_symlink()
    if exists_after or cache_exists_after:
        raise RuntimeError(
            "freshness cleanup left filesystem state behind: "
            f"path={path}, exists_after={exists_after}, "
            f"cache_exists_after={cache_exists_after}"
        )
    return {
        "path": str(path),
        "existed_before_cleanup": existed_before,
        "cmake_cache_existed_before_cleanup": cache_existed_before,
        "exists_after_cleanup": exists_after,
        "cmake_cache_exists_before_configure": cache_exists_after,
        "freshness_verified": not exists_after and not cache_exists_after,
    }


def run_command(
    command: list[str], cwd: Path, log_path: Path, env: dict[str, str] | None = None
) -> int:
    """@brief Run one replay command and persist its combined output.

    @param command Executable and arguments passed without a shell.
    @param cwd Working directory for the child process.
    @param log_path Evidence file replaced with command and combined output.
    @param env Optional complete child environment; ``None`` inherits the caller.
    @return Child-process exit status without raising for nonzero results.
    @throws OSError If the command cannot start or the log cannot be written.
    @note Standard error is merged into standard output. The function changes no
      process-global environment and has no shell-expansion behavior.
    """

    proc = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )
    log_path.write_text(
        "$ " + shlex.join(command) + "\n" + proc.stdout,
        encoding="utf-8",
    )
    return proc.returncode


def capture_cmake_version(
    cmake_executable: str, cwd: Path, log_path: Path
) -> tuple[int, str]:
    """@brief Record the exact CMake executable/version used by the smoke.

    @param cmake_executable Executable path or command selected by the caller.
    @param cwd Working directory for the version query.
    @param log_path Evidence file replaced with command and combined output.
    @return Child status and the first output line, or an empty version string.
    @throws OSError If the command cannot start or the log cannot be written.
    @note The query has no repository/build mutation side effects; standard error
      is merged into standard output for reproducible diagnostics.
    """

    command = [cmake_executable, "--version"]
    proc = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    log_path.write_text(
        "$ " + shlex.join(command) + "\n" + proc.stdout,
        encoding="utf-8",
    )
    lines = proc.stdout.splitlines()
    return proc.returncode, lines[0] if lines else ""


def configure_fresh_producer(
    cmake_executable: str,
    repo: Path,
    build: Path,
    config: str,
    generator: str,
    build_testing: str,
    log_path: Path,
) -> tuple[int, dict[str, Any], list[str]]:
    """@brief Configure a clean top-level Photospider producer tree.

    @param cmake_executable Exact CMake executable selected by the caller.
    @param repo Photospider source repository passed as the top-level source.
    @param build Dedicated producer build tree to delete and recreate.
    @param config Build configuration used as ``CMAKE_BUILD_TYPE`` when set.
    @param generator Optional CMake generator selected with ``-G``; an empty
      value preserves CMake's environment/default generator selection.
    @param build_testing ``ON`` or ``OFF`` for the producer's test targets.
    @param log_path Evidence file replaced with the configure command/output.
    @return Configure process exit status, real pre-configure cleanup
      observation, and the exact executed command vector.
    @throws ValueError If ``build`` is the source tree or one of its ancestors.
    @throws OSError If cleanup, command startup, or log writing fails.
    @throws RuntimeError If cleanup returns while the build tree or cache remains.
    @note Cleanup is intentionally destructive only below the explicit
      ``--build`` path. The later product build still names the real
      ``photospider`` target, so disabling tests cannot bypass the product.
    """

    if build == repo or build in repo.parents:
        raise ValueError(
            f"refusing to remove source tree or ancestor as build path: {build}"
        )
    cleanup = strict_remove_tree(build)
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
    return run_command(command, repo, log_path), cleanup, command


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
      :func:`main` owns cleanup according to ``--retain-artifacts``.
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
                "file(GENERATE",
                '    OUTPUT "${CMAKE_BINARY_DIR}/photospider_consumer_target_$<CONFIG>.txt"',
                '    CONTENT "$<TARGET_FILE:photospider_consumer>\\n")',
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
    """@brief Render installed evidence paths without assuming prefix containment.

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
    @throws None This is a pure exact-prefix/suffix detector.
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
    consumer_build: Path, requested_config: str
) -> dict[str, Any]:
    """@brief Locate the built consumer using CMake target-file manifests.

    Manifest paths generated from ``$<TARGET_FILE:photospider_consumer>`` are
    authoritative for Xcode, Ninja Multi-Config, Visual Studio, and single-config
    generators. A filename fallback remains diagnostic protection for malformed
    or missing manifests.

    @param consumer_build Configured consumer build directory.
    @param requested_config Optional configuration passed to ``cmake --build``.
    @return Manifest inventory, declared/fallback candidates, and selected file.
    @throws OSError If an existing manifest or build tree cannot be read.
    @note The function is read-only and accepts only regular existing files as
      runnable candidates. Fresh consumer builds prevent stale-config selection.
    """

    manifests = sorted(consumer_build.glob(TARGET_MANIFEST_GLOB))
    if requested_config:
        preferred_name = f"photospider_consumer_target_{requested_config}.txt"
        manifests.sort(key=lambda path: path.name != preferred_name)
    declared_paths: list[Path] = []
    for manifest in manifests:
        value = manifest.read_text(encoding="utf-8").strip()
        if value:
            declared_paths.append(Path(value))

    fallback_paths = sorted(
        path
        for name in ("photospider_consumer", "photospider_consumer.exe")
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


def display_optional_path(value: str) -> str:
    """@brief Render one optional evidence path without ambiguous whitespace.

    @param value Filesystem path recorded by a runtime detector, or an empty
      string when no path was found.
    @return ``value`` unchanged when nonempty, otherwise ``<not found>``.
    @throws None.
    @note This pure formatter prevents an absent path from becoming invisible
      trailing whitespace in structured Markdown evidence.
    """

    return value if value else "<not found>"


def display_path_list(values: list[str]) -> str:
    """@brief Render a path inventory with an explicit empty-state marker.

    @param values Ordered filesystem paths produced by a runtime detector.
    @return Comma-separated paths, or ``<not found>`` when the list is empty.
    @throws TypeError If a caller supplies a non-string list element.
    @note This pure formatter preserves detector order and never appends spaces.
    """

    return ", ".join(values) if values else "<not found>"


def capture_git_boundary(repo: Path) -> dict[str, Any]:
    """@brief Capture the tracked and ordinary-untracked source boundary.

    @param repo Git working tree used as the producer source.
    @return Commit id, patch SHA-256 values, tracked path inventories, and
      content hashes for every ordinary untracked file observed before
      verification starts.
    @throws OSError If Git cannot be launched.
    @throws RuntimeError If any boundary query exits nonzero.
    @note Queries are read-only and do not create index/tree objects. Ignored
      build/evidence files remain outside the boundary, while ordinary
      untracked source files are bound by path and content hash.
    """

    commands = {
        "head": ["git", "rev-parse", "HEAD"],
        "staged_patch": ["git", "diff", "--cached", "--binary", "--no-ext-diff"],
        "unstaged_patch": ["git", "diff", "--binary", "--no-ext-diff"],
        "staged_paths": ["git", "diff", "--cached", "--name-only", "--no-ext-diff"],
        "unstaged_paths": ["git", "diff", "--name-only", "--no-ext-diff"],
        "untracked_paths": [
            "git",
            "ls-files",
            "--others",
            "--exclude-standard",
            "-z",
        ],
    }
    outputs: dict[str, bytes] = {}
    for name, command in commands.items():
        proc = subprocess.run(
            command,
            cwd=repo,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"git boundary query failed ({name}, exit={proc.returncode}): "
                + proc.stderr.decode("utf-8", errors="replace")
            )
        outputs[name] = proc.stdout
    untracked_paths = [
        part.decode("utf-8") for part in outputs["untracked_paths"].split(b"\0") if part
    ]
    untracked_sha256: dict[str, str] = {}
    for relative_path in untracked_paths:
        source_path = repo / relative_path
        if not source_path.is_file():
            raise RuntimeError(
                f"ordinary untracked source is not a regular file: {relative_path}"
            )
        untracked_sha256[relative_path] = hashlib.sha256(
            source_path.read_bytes()
        ).hexdigest()
    return {
        "head_commit": outputs["head"].decode("utf-8").strip(),
        "staged_patch_sha256": hashlib.sha256(outputs["staged_patch"]).hexdigest(),
        "unstaged_patch_sha256": hashlib.sha256(outputs["unstaged_patch"]).hexdigest(),
        "staged_paths": [
            line
            for line in outputs["staged_paths"]
            .decode("utf-8", errors="replace")
            .splitlines()
            if line
        ],
        "unstaged_paths": [
            line
            for line in outputs["unstaged_paths"]
            .decode("utf-8", errors="replace")
            .splitlines()
            if line
        ],
        "untracked_paths": untracked_paths,
        "untracked_file_sha256": untracked_sha256,
    }


def resolve_executable(executable: str) -> str:
    """@brief Resolve one requested executable to an absolute real path.

    @param executable Absolute/relative path or command name selected by CLI.
    @return Resolved executable path when discoverable, otherwise input text.
    @throws OSError If platform path resolution fails.
    @note ``shutil.which`` is used for command names; no executable is launched.
    """

    candidate = Path(executable)
    if candidate.is_absolute() or candidate.parent != Path("."):
        return str(candidate.resolve())
    located = shutil.which(executable)
    return str(Path(located).resolve()) if located else executable


def parse_recorded_command(log_path: Path) -> list[str]:
    """@brief Parse the shell-quoted command header written by ``run_command``.

    @param log_path Command log whose first command line starts with ``$ ``.
    @return Parsed argument vector, or an empty list for a deliberate no-command
      log such as existing-producer mode.
    @throws OSError If the log cannot be read.
    @throws ValueError If a command header contains invalid shell quoting.
    @note The function reads only the first line and never executes its content.
    """

    first_line = log_path.read_text(encoding="utf-8").splitlines()[0]
    if not first_line.startswith("$ "):
        return []
    return shlex.split(first_line[2:])


def make_run_environment(
    actual: dict[str, Any],
    started_at_utc: str,
    finished_at_utc: str,
    invocation_argv: list[str],
    invocation_cwd: Path,
) -> dict[str, Any]:
    """@brief Build environment evidence directly from this run's observations.

    @param actual Real source boundary, command, cache, producer, and path data.
    @param started_at_utc ISO-8601 UTC start time captured before verification.
    @param finished_at_utc ISO-8601 UTC end time captured after final cleanup.
    @param invocation_argv Exact Python process argument vector for replay.
    @param invocation_cwd Working directory from which the verifier was invoked.
    @return Structured environment payload used by JSON and Markdown evidence.
    @throws KeyError If ``actual`` lacks a required observation.
    @throws TypeError If invocation arguments are not strings.
    @note The payload is generated, not read from an older environment file.
    """

    paths = actual["paths"]
    return {
        "schema_version": 1,
        "started_at_utc": started_at_utc,
        "finished_at_utc": finished_at_utc,
        "source_boundary": actual["source_boundary"],
        "platform": {
            "system": platform.system(),
            "machine": platform.machine(),
            "description": platform.platform(),
        },
        "cmake": {
            "requested_executable": paths["cmake_executable"],
            "resolved_executable": paths["cmake_resolved_executable"],
            "version": paths["cmake_version"],
        },
        "paths": {
            "repo": paths["repo"],
            "producer_build": paths["build"],
            "install_prefix": paths["prefix"],
            "consumer_source": paths["consumer_source"],
            "consumer_build": paths["consumer_build"],
            "evidence_output": paths["evidence_output"],
        },
        "producer": actual["producer"],
        "producer_cache": actual["producer_cache"],
        "commands": actual["command_lines"],
        "invocation": {
            "cwd": str(invocation_cwd),
            "argv": invocation_argv,
            "replay_command": shlex.join(invocation_argv),
        },
    }


def inspect_environment_consistency(
    environment: dict[str, Any],
    actual: dict[str, Any],
    recorded_commands: dict[str, list[str]],
) -> dict[str, bool]:
    """@brief Compare environment fields with actual paths, logs, and cache.

    @param environment Generated run-environment payload.
    @param actual Real observations before the environment payload is attached.
    @param recorded_commands Commands parsed back from this run's command logs.
    @return Independent booleans for timestamp, boundary, path, CMake, command,
      producer-cache, and freshness consistency.
    @throws KeyError If either payload lacks a required field.
    @throws ValueError If an ISO timestamp is malformed; the error is converted
      into a false timestamp check.
    @note No expected value is copied into actual. The producer cache and logs
      are independently read from artifacts created by the selected CMake run.
    """

    try:
        started = datetime.fromisoformat(environment["started_at_utc"])
        finished = datetime.fromisoformat(environment["finished_at_utc"])
        timestamps_valid = (
            started.tzinfo is not None
            and finished.tzinfo is not None
            and started <= finished
        )
    except ValueError:
        timestamps_valid = False

    boundary = environment["source_boundary"]
    boundary_shape_valid = bool(
        re.fullmatch(r"[0-9a-f]{40}", boundary["head_commit"])
        and re.fullmatch(r"[0-9a-f]{64}", boundary["staged_patch_sha256"])
        and re.fullmatch(r"[0-9a-f]{64}", boundary["unstaged_patch_sha256"])
        and boundary["untracked_paths"] == list(boundary["untracked_file_sha256"])
        and all(
            re.fullmatch(r"[0-9a-f]{64}", digest)
            for digest in boundary["untracked_file_sha256"].values()
        )
    )
    actual_paths = actual["paths"]
    environment_paths = environment["paths"]
    paths_match = environment_paths == {
        "repo": actual_paths["repo"],
        "producer_build": actual_paths["build"],
        "install_prefix": actual_paths["prefix"],
        "consumer_source": actual_paths["consumer_source"],
        "consumer_build": actual_paths["consumer_build"],
        "evidence_output": actual_paths["evidence_output"],
    }
    cmake_matches = environment["cmake"] == {
        "requested_executable": actual_paths["cmake_executable"],
        "resolved_executable": actual_paths["cmake_resolved_executable"],
        "version": actual_paths["cmake_version"],
    }
    invocation = environment["invocation"]
    replay_matches = invocation["replay_command"] == shlex.join(invocation["argv"])
    commands_match = environment["commands"] == actual["command_lines"]
    command_logs_match = all(
        recorded_commands[name] == command
        for name, command in environment["commands"].items()
    )
    producer_configure_command = environment["commands"]["producer_configure"]
    if actual["producer"]["configured_by_smoke"]:
        producer_log_matches = bool(producer_configure_command) and (
            recorded_commands["producer_configure"] == producer_configure_command
        )
    else:
        producer_log_matches = (
            producer_configure_command == []
            and recorded_commands["producer_configure"] == []
        )

    cache = actual["producer_cache"]
    cache_cmake = cache["cmake_command"]
    resolved_cmake = environment["cmake"]["resolved_executable"]
    cmake_cache_matches = bool(cache_cmake and resolved_cmake) and (
        Path(cache_cmake).resolve() == Path(resolved_cmake).resolve()
    )
    cache_paths_match = (
        Path(cache["home_directory"]).resolve()
        == Path(environment_paths["repo"]).resolve()
        and Path(cache["cachefile_dir"]).resolve()
        == Path(environment_paths["producer_build"]).resolve()
    )
    requested_config = actual["producer"]["requested_config"]
    requested_generator = actual["producer"]["requested_generator"]
    configuration_types = [
        value for value in cache["configuration_types"].split(";") if value
    ]
    generator_matches = (
        not requested_generator or cache["generator"] == requested_generator
    )
    requested_config_matches = not requested_config or (
        cache["build_type"] == requested_config
        or requested_config in configuration_types
    )
    cache_configuration_matches = (
        generator_matches
        and requested_config_matches
        and cache["build_testing"] == actual["producer"]["build_testing"]
    )
    producer_freshness = actual["freshness"]["producer_build"]
    freshness_matches = (
        not actual["producer"]["configured_by_smoke"] and producer_freshness is None
    ) or (
        actual["producer"]["configured_by_smoke"]
        and producer_freshness is not None
        and producer_freshness["path"] == environment_paths["producer_build"]
        and producer_freshness["freshness_verified"]
    )
    return {
        "timestamps_are_real_and_ordered": timestamps_valid,
        "source_boundary_matches_actual": (
            boundary == actual["source_boundary"] and boundary_shape_valid
        ),
        "paths_match_actual": paths_match,
        "cmake_identity_matches_actual": cmake_matches,
        "exact_replay_command_matches_argv": replay_matches,
        "command_vectors_match_actual": commands_match,
        "command_logs_match_environment": command_logs_match,
        "producer_configure_log_matches_environment": producer_log_matches,
        "producer_cache_cmake_matches_environment": cmake_cache_matches,
        "producer_cache_paths_match_environment": cache_paths_match,
        "producer_cache_configuration_matches_actual": cache_configuration_matches,
        "producer_freshness_matches_environment": freshness_matches,
        "producer_cache_payload_matches_environment": (
            environment["producer_cache"] == actual["producer_cache"]
        ),
    }


def render_environment_markdown(
    environment: dict[str, Any], consistency: dict[str, bool]
) -> str:
    """@brief Render generated strict-fresh environment evidence.

    @param environment Structured payload derived from this run's actual data.
    @param consistency Programmatic comparisons against logs/cache/actual.
    @return Reader-facing Markdown without a trailing newline.
    @throws KeyError If a required environment field is absent.
    @note The command audit is explicitly excluded because it is a separate run
      with its own command and timestamps.
    """

    boundary = environment["source_boundary"]
    paths = environment["paths"]
    staged_paths = boundary["staged_paths"] or ["<none>"]
    unstaged_paths = boundary["unstaged_paths"] or ["<none>"]
    untracked_paths = boundary["untracked_paths"] or ["<none>"]
    producer_command = environment["commands"]["producer_configure"]
    return "\n".join(
        [
            "# CMake strict-fresh producer environment",
            "",
            "本文件由 `phase4_static_product_consumer_smoke.py` 根据本次真实",
            "`actual`、进程参数、command log 与 producer cache 自动生成。",
            "",
            "## 运行时间与平台",
            "",
            f"- 开始时间（UTC）：`{environment['started_at_utc']}`",
            f"- 结束时间（UTC）：`{environment['finished_at_utc']}`",
            f"- 系统：`{environment['platform']['system']}`",
            f"- 架构：`{environment['platform']['machine']}`",
            f"- 平台说明：`{environment['platform']['description']}`",
            "",
            "## 源码边界",
            "",
            f"- HEAD commit：`{boundary['head_commit']}`",
            f"- staged patch SHA-256：`{boundary['staged_patch_sha256']}`",
            f"- unstaged patch SHA-256：`{boundary['unstaged_patch_sha256']}`",
            "- staged paths：" + ", ".join(f"`{path}`" for path in staged_paths),
            "- unstaged paths：" + ", ".join(f"`{path}`" for path in unstaged_paths),
            "- ordinary untracked paths："
            + ", ".join(f"`{path}`" for path in untracked_paths),
            "- ordinary untracked file SHA-256："
            + (
                ", ".join(
                    f"`{path}`=`{digest}`"
                    for path, digest in boundary["untracked_file_sha256"].items()
                )
                or "<none>"
            ),
            "",
            "## CMake 与路径",
            "",
            "- 请求的 CMake：" f"`{environment['cmake']['requested_executable']}`",
            "- 解析后的 CMake：" f"`{environment['cmake']['resolved_executable']}`",
            f"- CMake 版本：`{environment['cmake']['version']}`",
            f"- producer build：`{paths['producer_build']}`",
            f"- install prefix：`{paths['install_prefix']}`",
            f"- consumer source：`{paths['consumer_source']}`",
            f"- consumer build：`{paths['consumer_build']}`",
            f"- evidence output：`{paths['evidence_output']}`",
            "",
            "## 精确进程内重放命令",
            "",
            "```bash",
            environment["invocation"]["replay_command"],
            "```",
            "",
            "producer configure 实际命令：",
            "",
            "```bash",
            shlex.join(producer_command) if producer_command else "<not run>",
            "```",
            "",
            "## 程序性一致性检查",
            "",
            *[
                f"- {name}: {'PASS' if value else 'FAIL'}"
                for name, value in consistency.items()
            ],
            "",
            "command/provider audit 是独立运行，不计入上述起止时间；其命令与",
            "时间必须记录在相邻 `command_compatibility_audit/` 证据中。",
        ]
    )


def inspect_detector_contract() -> dict[str, bool]:
    """@brief Exercise executable and exported-property detectors synthetically.

    @return Contract booleans covering non-Windows multi-config, Windows `.exe`,
      missing-output rejection, explicit missing-path rendering, exact
      target-property selection, link-only parsing, strict cleanup failure/
      residual detection, and stale environment path/cache rejection.
    @throws OSError If the system temporary directory cannot be created or used.
    @note Creates only auto-cleaned temporary files. No repository or evidence
      path is changed, and no executable is launched.
    """

    with tempfile.TemporaryDirectory(prefix="photospider-phase4-detector-") as raw:
        root = Path(raw)
        xcode_build = root / "xcode-build"
        xcode_executable = xcode_build / "RelWithDebInfo" / "photospider_consumer"
        xcode_executable.parent.mkdir(parents=True)
        xcode_executable.touch()
        (xcode_build / "photospider_consumer_target_RelWithDebInfo.txt").write_text(
            xcode_executable.as_posix() + "\n", encoding="utf-8"
        )
        xcode_result = find_consumer_executable(xcode_build, "RelWithDebInfo")

        windows_build = root / "visual-studio-build"
        windows_executable = windows_build / "Debug" / "photospider_consumer.exe"
        windows_executable.parent.mkdir(parents=True)
        windows_executable.touch()
        (windows_build / "photospider_consumer_target_Debug.txt").write_text(
            windows_executable.as_posix() + "\n", encoding="utf-8"
        )
        windows_result = find_consumer_executable(windows_build, "Debug")
        missing_result = find_consumer_executable(root / "missing-build", "Debug")

        synthetic_targets = r"""set_target_properties(Unrelated::target PROPERTIES
  INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:wrong>"
)
set_target_properties(Photospider::photospider PROPERTIES
  INTERFACE_LINK_LIBRARIES "\$<LINK_ONLY:Threads::Threads>;\$<LINK_ONLY:yaml-cpp::yaml-cpp>"
)
"""
        property_value = exported_target_property(
            synthetic_targets, EXPORTED_TARGET, "INTERFACE_LINK_LIBRARIES"
        )
        property_entries = split_cmake_list(property_value)

        failing_cleanup = root / "failing-cleanup"
        failing_cleanup.mkdir()
        cleanup_failure_propagated = False
        try:
            strict_remove_tree(
                failing_cleanup,
                Mock(side_effect=PermissionError("synthetic removal failure")),
            )
        except PermissionError:
            cleanup_failure_propagated = True

        residual_cleanup = root / "residual-cleanup"
        residual_cleanup.mkdir()
        cleanup_residual_detected = False
        try:
            strict_remove_tree(residual_cleanup, Mock(return_value=None))
        except RuntimeError:
            cleanup_residual_detected = True

    synthetic_commands = {
        "cmake_version": ["/tool/cmake", "--version"],
        "producer_configure": [
            "/tool/cmake",
            "-S",
            "/repo",
            "-B",
            "/build",
            "-DBUILD_TESTING=OFF",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        ],
        "build_photospider": [
            "/tool/cmake",
            "--build",
            "/build",
            "--target",
            "photospider",
            "--config",
            "RelWithDebInfo",
        ],
        "install": [
            "/tool/cmake",
            "--install",
            "/build",
            "--prefix",
            "/evidence/install-prefix",
            "--config",
            "RelWithDebInfo",
        ],
        "consumer_configure": [
            "/tool/cmake",
            "-S",
            "/evidence/consumer-src",
            "-B",
            "/evidence/consumer-build",
        ],
        "consumer_build": [
            "/tool/cmake",
            "--build",
            "/evidence/consumer-build",
        ],
        "consumer_run": ["/evidence/consumer-build/photospider_consumer"],
    }
    synthetic_boundary = {
        "head_commit": "a" * 40,
        "staged_patch_sha256": "b" * 64,
        "unstaged_patch_sha256": "c" * 64,
        "staged_paths": ["CMakeLists.txt"],
        "unstaged_paths": ["tests/verifier.py"],
        "untracked_paths": ["tests/new_probe.py"],
        "untracked_file_sha256": {"tests/new_probe.py": "d" * 64},
    }
    synthetic_actual = {
        "source_boundary": synthetic_boundary,
        "paths": {
            "repo": "/repo",
            "cmake_executable": "/tool/cmake",
            "cmake_resolved_executable": "/tool/cmake",
            "cmake_version": "cmake version 3.16.9",
            "build": "/build",
            "prefix": "/evidence/install-prefix",
            "consumer_source": "/evidence/consumer-src",
            "consumer_build": "/evidence/consumer-build",
            "evidence_output": "/evidence",
        },
        "producer": {
            "configured_by_smoke": True,
            "build_testing": "OFF",
            "requested_config": "RelWithDebInfo",
            "requested_generator": "",
            "target_built": "photospider",
        },
        "producer_cache": {
            "cmake_command": "/tool/cmake",
            "home_directory": "/repo",
            "cachefile_dir": "/build",
            "generator": "Unix Makefiles",
            "configuration_types": "",
            "build_type": "RelWithDebInfo",
            "build_testing": "OFF",
        },
        "freshness": {
            "producer_build": {
                "path": "/build",
                "freshness_verified": True,
            }
        },
        "command_lines": synthetic_commands,
    }
    synthetic_environment = make_run_environment(
        synthetic_actual,
        "2026-07-10T00:00:00+00:00",
        "2026-07-10T00:00:01+00:00",
        ["python3", "verifier.py", "--build", "/build"],
        Path("/repo"),
    )
    consistent_environment = inspect_environment_consistency(
        synthetic_environment, synthetic_actual, synthetic_commands
    )
    stale_path_environment = json.loads(json.dumps(synthetic_environment))
    stale_path_environment["paths"]["producer_build"] = "/old-build"
    stale_path_consistency = inspect_environment_consistency(
        stale_path_environment, synthetic_actual, synthetic_commands
    )
    stale_cache_actual = json.loads(json.dumps(synthetic_actual))
    stale_cache_actual["producer_cache"]["cmake_command"] = "/old/cmake"
    stale_cache_consistency = inspect_environment_consistency(
        synthetic_environment, stale_cache_actual, synthetic_commands
    )

    return {
        "finds_non_windows_multiconfig_target_manifest": xcode_result["selected"]
        == xcode_executable.as_posix()
        and xcode_result["selected_from_manifest"],
        "finds_windows_multiconfig_exe_target_manifest": windows_result["selected"]
        == windows_executable.as_posix()
        and windows_result["selected_from_manifest"],
        "rejects_missing_executable": missing_result["selected"] == "",
        "selects_exact_exported_target_property": property_entries
        == [
            "$<LINK_ONLY:Threads::Threads>",
            "$<LINK_ONLY:yaml-cpp::yaml-cpp>",
        ],
        "recognizes_link_only_entries": all(
            unwrap_link_only(entry)[1] for entry in property_entries
        ),
        "renders_missing_manifest_and_executable_paths_explicitly": (
            display_path_list([]) == "<not found>"
            and display_optional_path("") == "<not found>"
        ),
        "propagates_strict_cleanup_failure": cleanup_failure_propagated,
        "detects_strict_cleanup_residual": cleanup_residual_detected,
        "accepts_consistent_generated_environment": all(
            consistent_environment.values()
        ),
        "detects_stale_environment_build_path": not stale_path_consistency[
            "paths_match_actual"
        ],
        "detects_stale_environment_cmake_cache": not stale_cache_consistency[
            "producer_cache_cmake_matches_environment"
        ],
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


def make_compare(actual: dict[str, Any]) -> tuple[bool, str]:
    """@brief Compare real install/build/run evidence with package invariants.

    @param actual Evidence assembled from command results, install tree, exported
      properties, detector contracts, and the generated consumer.
    @return Overall pass state and deterministic PASS/FAIL comparison text.
    @throws KeyError If the caller provides an incomplete evidence schema.
    @note The comparison never substitutes expected values into ``actual`` and
      performs no filesystem writes.
    """

    install = actual["install_tree"]
    commands = actual["commands"]
    link_contract = install["export_interface_link_libraries"]["classification"]
    compiled_headers = sorted(
        "include/" + line.removeprefix("#include <").removesuffix(">")
        for line in actual["consumer"]["compiled_public_headers"]
    )
    required_freshness_directories = [
        "install_prefix",
        "consumer_source",
        "consumer_build",
    ]
    if actual["producer"]["configured_by_smoke"]:
        required_freshness_directories.append("producer_build")
    freshness = actual["freshness"]
    checks = {
        "selected CMake executable is runnable": commands["cmake_version"] == 0,
        "fresh producer configure succeeded when requested": (
            not actual["producer"]["configured_by_smoke"]
            or commands["producer_configure"] == 0
        ),
        "freshness-critical directories were strictly removed": all(
            freshness[name] is not None
            and freshness[name]["freshness_verified"]
            and not freshness[name]["exists_after_cleanup"]
            and not freshness[name]["cmake_cache_exists_before_configure"]
            for name in required_freshness_directories
        ),
        "generated environment matches actual logs and producer cache": all(
            actual["environment_consistency"].values()
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
        "exported target carries PHOTOSPIDER_STATIC": install[
            "export_has_static_compile_definition"
        ],
        "exported target uses install include root": install[
            "export_has_install_include_dir"
        ],
        "exported package is source-tree clean": install["export_omits_source_root"]
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
            not actual["producer"]["requested_generator"]
            or (
                actual["producer_cache"]["generator"]
                == actual["producer"]["requested_generator"]
                and actual["consumer"]["generator"]
                == actual["producer"]["requested_generator"]
            )
        ),
        "requested config belongs to each multi-config cache": (
            not actual["producer"]["requested_config"]
            or (
                not actual["producer_cache"]["configuration_types"]
                and not actual["consumer"]["configuration_types"]
            )
            or (
                actual["producer"]["requested_config"]
                in actual["producer_cache"]["configuration_types"].split(";")
                and actual["producer"]["requested_config"]
                in actual["consumer"]["configuration_types"].split(";")
            )
        ),
        "consumer build succeeded": commands["consumer_build"] == 0,
        "CMake target manifest resolves consumer executable": actual["consumer"][
            "executable_discovery"
        ]["selected_from_manifest"],
        "consumer executable ran successfully": commands["consumer_run"] == 0,
        "detector contract covers multi-config and exact export parsing": all(
            actual["detector_contract"].values()
        ),
    }
    passed = all(checks.values())
    lines = ["phase4_static_product_consumer_smoke"]
    lines.extend(f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items())
    if install["unexpected_headers"]:
        lines.append("unexpected installed headers:")
        lines.extend(f"- {header}" for header in install["unexpected_headers"])
    lines.append(f"overall={'PASS' if passed else 'FAIL'}")
    return passed, "\n".join(lines) + "\n"


def make_summary(out: Path, actual: dict[str, Any], passed: bool) -> str:
    """@brief Render reader-facing installed-consumer evidence.

    @param out Evidence directory whose files are linked in the summary.
    @param actual Real package, export, build, and runtime observations.
    @param passed Aggregate comparison result.
    @return Markdown summary without a trailing newline.
    @throws KeyError If ``actual`` does not match the evidence schema.
    @note This pure formatter does not inspect or write artifacts itself.
    """

    link_contract = actual["install_tree"]["export_interface_link_libraries"][
        "classification"
    ]
    link_entries = link_contract["link_only_entries"]
    dl_contract = link_contract["required_dependencies"]
    dl_rows = dl_contract["dl"]
    dl_rows_are_link_only = bool(dl_rows) and all(row["link_only"] for row in dl_rows)
    if dl_contract["dl_required_for_platform"]:
        if dl_rows_are_link_only:
            dl_summary = "required and exported as `$<LINK_ONLY:...>`"
        elif dl_rows:
            dl_summary = "required but not all exported entries are LINK_ONLY"
        else:
            dl_summary = "required but missing from the exported interface"
    elif dl_rows:
        dl_summary = "not required on this platform; optional exported entries are " + (
            "LINK_ONLY" if dl_rows_are_link_only else "not all LINK_ONLY"
        )
    else:
        dl_summary = "not required on this platform and legally absent from the export"
    executable_discovery = actual["consumer"]["executable_discovery"]
    cmake_executable_summary = display_optional_path(
        actual["paths"]["cmake_executable"]
    )
    manifest_summary = display_path_list(executable_discovery["manifest_files"])
    executable_summary = display_optional_path(executable_discovery["selected"])
    producer_build_testing_summary = display_optional_path(
        actual["producer"]["build_testing"]
    )
    archive_summary = display_path_list(
        actual["install_tree"]["static_product_archives"]
    )
    verified_freshness_directories = [
        name
        for name, observation in actual["freshness"].items()
        if observation is not None and observation["freshness_verified"]
    ]
    producer_objective = (
        [
            "Recreate and configure a fresh top-level producer with the selected",
            "CMake, then build and install the real `photospider` target.",
        ]
        if actual["producer"]["configured_by_smoke"]
        else ["Use the caller-provided configured producer build tree."]
    )
    return "\n".join(
        [
            "# codebase-refactor phase-4 static product consumer smoke",
            "",
            "## Test objective",
            "",
            *producer_objective,
            "",
            "Install the static Photospider package, consume it from a separate",
            "CMake project with `find_package(Photospider CONFIG REQUIRED)`,",
            "compile every public header, derive the executable path from",
            "`$<TARGET_FILE:photospider_consumer>`, run it, and parse the exact",
            "installed target's link interface.",
            "",
            "## Evidence files",
            "",
            f"- `expected.json`: `{out / 'expected.json'}`",
            f"- `actual.json`: `{out / 'actual.json'}`",
            f"- `compare.log`: `{out / 'compare.log'}`",
            f"- `environment.md`: `{out / 'environment.md'}`",
            f"- `cmake_version.log`: `{out / 'cmake_version.log'}`",
            f"- `producer_configure.log`: `{out / 'producer_configure.log'}`",
            f"- `build_photospider.log`: `{out / 'build_photospider.log'}`",
            f"- `install.log`: `{out / 'install.log'}`",
            f"- `consumer_configure.log`: `{out / 'consumer_configure.log'}`",
            f"- `consumer_build.log`: `{out / 'consumer_build.log'}`",
            f"- `consumer_run.log`: `{out / 'consumer_run.log'}`",
            "",
            "## Result",
            "",
            "- Run started (UTC): " + actual["run_environment"]["started_at_utc"],
            "- Run finished (UTC): " + actual["run_environment"]["finished_at_utc"],
            "- Source HEAD: " + actual["source_boundary"]["head_commit"],
            "- Staged patch SHA-256: "
            + actual["source_boundary"]["staged_patch_sha256"],
            "- Unstaged patch SHA-256: "
            + actual["source_boundary"]["unstaged_patch_sha256"],
            "- Ordinary untracked source files: "
            f"{len(actual['source_boundary']['untracked_paths'])}",
            "- CMake executable: " + cmake_executable_summary,
            "- Resolved CMake executable: "
            + actual["paths"]["cmake_resolved_executable"],
            "- CMake version: " + actual["paths"]["cmake_version"],
            "- Fresh producer configured by this run: "
            f"{actual['producer']['configured_by_smoke']}",
            "- Requested CMake generator: "
            + display_optional_path(actual["producer"]["requested_generator"]),
            "- Actual producer CMake generator: "
            + display_optional_path(actual["producer_cache"]["generator"]),
            "- Producer configuration types: "
            + display_optional_path(actual["producer_cache"]["configuration_types"]),
            "- Actual consumer CMake generator: "
            + display_optional_path(actual["consumer"]["generator"]),
            "- Consumer configuration types: "
            + display_optional_path(actual["consumer"]["configuration_types"]),
            "- Producer BUILD_TESTING cache value: " + producer_build_testing_summary,
            "- Strictly cleaned freshness directories: "
            + ", ".join(verified_freshness_directories),
            "- Public headers compiled by consumer: "
            f"{len(actual['consumer']['compiled_public_headers'])}",
            f"- Installed headers: {len(actual['install_tree']['headers'])}",
            "- Installed static archives: " + archive_summary,
            f"- Exported LINK_ONLY entries: {len(link_entries)}",
            "- Platform dl dependency: " + dl_summary,
            "- CMake target manifest files: " + manifest_summary,
            "- Consumer executable selected from CMake manifest: " + executable_summary,
            "- Unexpected installed headers: "
            f"{len(actual['install_tree']['unexpected_headers'])}",
            "- Environment consistency checks passing: "
            f"{sum(actual['environment_consistency'].values())}/"
            f"{len(actual['environment_consistency'])}",
            "- Generated environment Markdown SHA-256: "
            + actual["environment_markdown_sha256"],
            f"- Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "## Interpretation",
            "",
            "When fresh producer configuration is requested, a passing result",
            "first proves that the producer build, install prefix, consumer",
            "source, and consumer build paths were absent after strict cleanup",
            "and had no pre-configure `CMakeCache.txt`; deletion exceptions or",
            "residual paths are fatal. It then proves the selected CMake",
            "configured the top-level project, built the real `photospider`",
            "target, installed it, and then configured,",
            "built, and ran an external embedded consumer. Without that option,",
            "the smoke retains its existing-build behavior. In both modes it",
            "also proves the real exported `Photospider::photospider` target",
            "carries Threads, yaml-cpp, and OpenCV dependencies as",
            "`$<LINK_ONLY:...>` entries rather than relying on source-text hits.",
            "A dl entry is required and described as LINK_ONLY only on platforms",
            "whose",
            "`CMAKE_DL_LIBS` expands nonempty; an empty Apple/Windows export is",
            "a valid platform result.",
            "The generated `environment.md` is part of the comparison contract:",
            "its timestamps, source patch boundary, CMake identity, producer/",
            "install/consumer paths, exact invocation, command-log headers, and",
            "producer cache values must agree with this run's actual evidence.",
            "When `--generator` selects a multi-configuration generator, the",
            "comparison additionally proves that both producer and consumer",
            "used that generator, that the requested configuration appears in",
            "both configuration-type caches, and that the configuration-specific",
            "target manifest selected the executable that actually ran.",
            "The separate command/provider audit has its own execution window",
            "and is intentionally not folded into these timestamps.",
        ]
    )


def write_json(path: Path, payload: dict[str, Any]) -> None:
    """@brief Persist deterministic JSON evidence.

    @param path Destination file whose parent directory already exists.
    @param payload JSON-serializable evidence object.
    @return None.
    @throws OSError If the destination cannot be written.
    @throws TypeError If ``payload`` is not serializable.
    @note Replaces only ``path`` and appends one final newline.
    """

    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    """@brief Optionally configure, then build/install/consume the package.

    @return Process status ``0`` when every package invariant passes, else ``1``.
    @throws OSError If commands cannot start or evidence/artifacts are
      inaccessible.
    @throws RuntimeError If a freshness or final cleanup leaves a path/cache.
    @throws ValueError If fresh-producer mode receives a build path that could
      delete the source tree or an evidence path below the deleted build tree.
    @note Fresh-producer mode recreates ``--build`` with the selected CMake.
      Every mode recreates transient install/consumer directories below
      ``--out`` and removes those by default after observations are captured,
      while retaining command logs and expected/actual/compare/summary evidence.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--out", required=True)
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
    parser.add_argument("--retain-artifacts", action="store_true")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    build = Path(args.build).resolve()
    out = Path(args.out).resolve()
    started_at_utc = datetime.now(timezone.utc).isoformat(timespec="microseconds")
    source_boundary = capture_git_boundary(repo)
    invocation_argv = [sys.executable, *sys.argv]
    invocation_cwd = Path.cwd().resolve()
    if args.configure_fresh_producer and (out == build or build in out.parents):
        raise ValueError(
            "fresh-producer evidence output must be outside the build tree: "
            f"build={build}, out={out}"
        )
    out.mkdir(parents=True, exist_ok=True)

    prefix = out / "install-prefix"
    consumer_src = out / "consumer-src"
    consumer_build = out / "consumer-build"
    freshness: dict[str, dict[str, Any] | None] = {
        "producer_build": None,
        "install_prefix": strict_remove_tree(prefix),
        "consumer_source": strict_remove_tree(consumer_src),
        "consumer_build": strict_remove_tree(consumer_build),
    }
    compiled_public_headers = write_consumer_project(repo, consumer_src)

    cmake_version_code, cmake_version = capture_cmake_version(
        args.cmake_executable, repo, out / "cmake_version.log"
    )
    producer_configure_code: int | None = None
    producer_configure_command: list[str] = []
    if args.configure_fresh_producer:
        (
            producer_configure_code,
            freshness["producer_build"],
            producer_configure_command,
        ) = configure_fresh_producer(
            args.cmake_executable,
            repo,
            build,
            args.config,
            args.generator,
            args.producer_build_testing,
            out / "producer_configure.log",
        )
    else:
        (out / "producer_configure.log").write_text(
            "fresh producer configure not requested; existing build tree used\n",
            encoding="utf-8",
        )

    build_product_command = [
        args.cmake_executable,
        "--build",
        str(build),
        "--target",
        "photospider",
    ]
    if args.config:
        build_product_command.extend(["--config", args.config])
    build_product_code = run_command(
        build_product_command, repo, out / "build_photospider.log"
    )

    install_command = [
        args.cmake_executable,
        "--install",
        str(build),
        "--prefix",
        str(prefix),
    ]
    if args.config:
        install_command.extend(["--config", args.config])
    install_code = run_command(install_command, repo, out / "install.log")

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
    configure_code = run_command(
        configure_command, repo, out / "consumer_configure.log"
    )

    build_command = [args.cmake_executable, "--build", str(consumer_build)]
    if args.config:
        build_command.extend(["--config", args.config])
    build_code = run_command(build_command, repo, out / "consumer_build.log")

    executable_discovery = find_consumer_executable(consumer_build, args.config)
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
    consumer_run_command: list[str] = []
    if executable is not None and executable.is_file():
        consumer_run_command = [str(executable)]
        run_code = run_command(consumer_run_command, repo, out / "consumer_run.log")
    else:
        (out / "consumer_run.log").write_text(
            "consumer executable not found; discovery="
            + json.dumps(executable_discovery, sort_keys=True)
            + "\n",
            encoding="utf-8",
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
    producer_cache = {
        "cmake_command": cmake_cache_value(build, "CMAKE_COMMAND"),
        "home_directory": cmake_cache_value(build, "CMAKE_HOME_DIRECTORY"),
        "cachefile_dir": cmake_cache_value(build, "CMAKE_CACHEFILE_DIR"),
        "generator": cmake_cache_value(build, "CMAKE_GENERATOR"),
        "configuration_types": cmake_cache_value(build, "CMAKE_CONFIGURATION_TYPES"),
        "build_type": cmake_cache_value(build, "CMAKE_BUILD_TYPE"),
        "build_testing": producer_build_testing,
    }
    command_lines = {
        "cmake_version": [args.cmake_executable, "--version"],
        "producer_configure": producer_configure_command,
        "build_photospider": build_product_command,
        "install": install_command,
        "consumer_configure": configure_command,
        "consumer_build": build_command,
        "consumer_run": consumer_run_command,
    }
    recorded_commands = {
        "cmake_version": parse_recorded_command(out / "cmake_version.log"),
        "producer_configure": parse_recorded_command(out / "producer_configure.log"),
        "build_photospider": parse_recorded_command(out / "build_photospider.log"),
        "install": parse_recorded_command(out / "install.log"),
        "consumer_configure": parse_recorded_command(out / "consumer_configure.log"),
        "consumer_build": parse_recorded_command(out / "consumer_build.log"),
        "consumer_run": parse_recorded_command(out / "consumer_run.log"),
    }
    detector_contract = inspect_detector_contract()

    if not args.retain_artifacts:
        strict_remove_tree(prefix)
        strict_remove_tree(consumer_src)
        strict_remove_tree(consumer_build)

    finished_at_utc = datetime.now(timezone.utc).isoformat(timespec="microseconds")
    actual = {
        "source_boundary": source_boundary,
        "commands": {
            "cmake_version": cmake_version_code,
            "producer_configure": producer_configure_code,
            "build_photospider": build_product_code,
            "install": install_code,
            "consumer_configure": configure_code,
            "consumer_build": build_code,
            "consumer_run": run_code,
        },
        "command_lines": command_lines,
        "install_tree": install_tree,
        "paths": {
            "repo": str(repo),
            "cmake_executable": args.cmake_executable,
            "cmake_resolved_executable": resolve_executable(args.cmake_executable),
            "cmake_version": cmake_version,
            "build": str(build),
            "prefix": str(prefix),
            "install_libdir": install_libdir,
            "package_cmake_dir": package_cmake_dir,
            "consumer_source": str(consumer_src),
            "consumer_build": str(consumer_build),
            "consumer_osx_architectures": osx_architectures,
            "platform_system": platform_system,
            "transient_artifacts_retained": args.retain_artifacts,
            "evidence_output": str(out),
        },
        "consumer": {
            "compiled_public_headers": compiled_public_headers,
            "generator": consumer_generator,
            "configuration_types": consumer_configuration_types,
            "executable_discovery": executable_discovery,
        },
        "producer": {
            "configured_by_smoke": args.configure_fresh_producer,
            "build_testing": producer_build_testing,
            "requested_config": args.config,
            "requested_generator": args.generator,
            "target_built": "photospider",
        },
        "producer_cache": producer_cache,
        "freshness": freshness,
        "detector_contract": detector_contract,
    }
    run_environment = make_run_environment(
        actual,
        started_at_utc,
        finished_at_utc,
        invocation_argv,
        invocation_cwd,
    )
    environment_consistency = inspect_environment_consistency(
        run_environment, actual, recorded_commands
    )
    environment_consistency["environment_file_matches_generated_payload"] = True
    environment_markdown = (
        render_environment_markdown(run_environment, environment_consistency) + "\n"
    )
    (out / "environment.md").write_text(
        environment_markdown,
        encoding="utf-8",
    )
    environment_consistency["environment_file_matches_generated_payload"] = (
        out / "environment.md"
    ).read_text(encoding="utf-8") == environment_markdown
    actual["run_environment"] = run_environment
    actual["environment_consistency"] = environment_consistency
    actual["environment_markdown_sha256"] = hashlib.sha256(
        environment_markdown.encode("utf-8")
    ).hexdigest()
    expected = {
        "consumer_target": EXPORTED_TARGET,
        "cmake_executable": "selected through --cmake-executable and recorded",
        "installed_archive": (
            "one platform-specific photospider static archive under the "
            "configured install prefix"
        ),
        "supported_archive_names": sorted(STATIC_PRODUCT_ARCHIVE_NAMES),
        "allowed_header_root": "include/photospider",
        "unexpected_headers": [],
        "required_link_only_dependencies": {
            "threads": "Threads::Threads",
            "yaml_cpp": "yaml-cpp::yaml-cpp",
            "opencv_components": list(REQUIRED_OPENCV_COMPONENTS),
            "dl": "required when CMAKE_DL_LIBS expands nonempty (Linux in CI)",
        },
        "apple_plain_framework_flags": list(APPLE_PRODUCT_LINK_FLAGS),
        "consumer_executable_path_source": "$<TARGET_FILE:photospider_consumer>",
        "generator_contract": (
            "--generator selects the same producer and consumer generator; "
            "a requested multi-config build configuration must exist in both "
            "CMAKE_CONFIGURATION_TYPES caches"
        ),
        "fresh_producer_mode": (
            "selected CMake configures a clean top-level build tree before "
            "the real photospider target build"
        ),
        "freshness_contract": {
            "required_directories": [
                "producer_build when --configure-fresh-producer is used",
                "install_prefix",
                "consumer_source",
                "consumer_build",
            ],
            "exists_after_cleanup": False,
            "cmake_cache_exists_before_configure": False,
            "cleanup_failure_or_residual": "fatal",
        },
        "environment_contract": {
            "generated_from_current_actual_and_invocation": True,
            "timestamps": "ordered ISO-8601 UTC start and finish",
            "source_boundary": (
                "HEAD plus staged/unstaged patch SHA-256 and ordinary "
                "untracked path/content SHA-256 inventory"
            ),
            "cmake_identity": "requested/resolved path and version",
            "paths": "producer build, install prefix, consumer source/build",
            "exact_replay_command": True,
            "programmatic_consistency": (
                "all fields match actual, command logs, and producer cache"
            ),
            "generated_markdown_sha256": "recorded from generated UTF-8 bytes",
        },
        "expected_command_exit_code": 0,
    }
    passed, compare = make_compare(actual)

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "compare.log").write_text(compare, encoding="utf-8")
    (out / "summary.md").write_text(
        make_summary(out, actual, passed) + "\n", encoding="utf-8"
    )
    print(f"overall={'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
