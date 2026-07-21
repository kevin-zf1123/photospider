#!/usr/bin/env python3
"""Build and run the provider-disabled operation replacement profile."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import stat
import subprocess


def run(command: list[str], cwd: pathlib.Path) -> None:
    """@brief Run one required nested-build command with inherited output.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing working directory for the child process.
    @return None after the command exits successfully.
    @throws OSError If the child process cannot be started.
    @throws subprocess.CalledProcessError If the command exits nonzero.
    @note The complete command is printed before execution, and inherited
      standard streams remain visible to CTest.
    """

    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def resolve_work_directory(
    work: pathlib.Path, repo: pathlib.Path
) -> pathlib.Path:
    """@brief Validate one destructive nested-build directory without
    following its symlinks for deletion.

    @param work Caller-selected directory whose prior tree may be removed.
    @param repo Photospider repository root that must remain untouched.
    @return Absolute work spelling after every destructive-path guard passes;
      the returned path is never replaced with a symlink target.
    @throws OSError If the current directory, either path, or an existing work
      component cannot be inspected.
    @throws RuntimeError If symlink resolution cannot complete.
    @throws ValueError If work contains parent traversal, resolves to the
      repository, any repository ancestor, or any filesystem root, or has a
      symlink in any existing path component.
    @note Canonical resolution is used only for protected-location comparison.
      Root-to-leaf lstat inspection is the final validation step, so deletion
      callers retain the original absolute spelling and never receive a
      resolved symlink target. The path is suitable only for caller-owned
      transient build content.
    """

    absolute_work = (
        work if work.is_absolute() else pathlib.Path.cwd() / work
    )
    if os.pardir in absolute_work.parts:
        raise ValueError(
            "refusing parent traversal in destructive work path: "
            f"{absolute_work}"
        )

    resolved_repo = repo.resolve()
    comparison_work = absolute_work.resolve()
    if comparison_work.parent == comparison_work:
        raise ValueError(
            f"refusing to remove filesystem root: {comparison_work}"
        )
    if (
        comparison_work == resolved_repo
        or comparison_work in resolved_repo.parents
    ):
        raise ValueError(
            "refusing to remove repository or ancestor as work path: "
            f"{comparison_work}"
        )

    components = (*reversed(absolute_work.parents), absolute_work)
    for component in components:
        try:
            metadata = component.lstat()
        except FileNotFoundError:
            break
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError(
                "refusing symlink component in destructive work path: "
                f"{component}"
            )
    return absolute_work


def remove_work_tree(work: pathlib.Path, repo: pathlib.Path) -> pathlib.Path:
    """@brief Remove one validated nested-build tree without hiding failure.

    @param work Caller-selected directory whose previous contents must vanish.
    @param repo Photospider repository root protected from recursive removal.
    @return Absolute, non-symlink-resolved work directory, absent when this
      function returns.
    @throws OSError If path inspection, resolution, or recursive removal fails.
    @throws ValueError If work contains parent traversal, resolves to a
      protected destructive path, or has any existing symlink component.
    @throws RuntimeError If symlink resolution cannot complete or recursive
      removal returns while the tree remains.
    @note Validation is repeated in this destructive helper so callers cannot
      accidentally separate safety checks from deletion. An existing tree is
      revalidated immediately before recursive removal to narrow the
      check/delete replacement window. Recursive removal always receives the
      validated absolute spelling, never a resolved symlink target. The helper
      never creates the returned directory.
    """

    validated_work = resolve_work_directory(work, repo)
    try:
        validated_work.lstat()
    except FileNotFoundError:
        return validated_work

    validated_work = resolve_work_directory(validated_work, repo)
    shutil.rmtree(validated_work)
    try:
        validated_work.lstat()
    except FileNotFoundError:
        return validated_work
    else:
        raise RuntimeError(
            f"nested provider build directory still exists: {validated_work}"
        )


def cmake_cache_values(build: pathlib.Path) -> dict[str, str]:
    """@brief Read exact assignments from one nested CMake cache.

    @param build Configured nested build directory containing CMakeCache.txt.
    @return Mapping from cache keys to their final serialized values.
    @throws OSError If the cache cannot be read.
    @throws UnicodeError If the cache is not valid UTF-8 text.
    @throws RuntimeError If the nested build has no regular CMake cache file.
    @note Comment and malformed lines are ignored; later duplicate keys win,
      matching CMake's effective final assignment.
    """

    cache_path = build / "CMakeCache.txt"
    if not cache_path.is_file():
        raise RuntimeError(f"nested provider build has no cache: {cache_path}")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        assignment, value = line.split("=", 1)
        if ":" not in assignment:
            continue
        key, _cache_type = assignment.split(":", 1)
        if key:
            values[key] = value
    return values


def validate_provider_disabled_cache(values: dict[str, str]) -> None:
    """@brief Validate the exact supported provider-disabled test profile.

    @param values Effective assignments parsed from the nested CMake cache.
    @return None after every required capability and target choice matches.
    @throws RuntimeError If a required cache entry is missing or has an
      unexpected value.
    @note OpenCV, YAML, graph CLI, and operation plugins intentionally remain
      enabled; only the repository operation provider and IPC are disabled.
    """

    expected = {
        "BUILD_TESTING": "ON",
        "PHOTOSPIDER_ENABLE_OPENCV": "ON",
        "PHOTOSPIDER_ENABLE_YAML": "ON",
        "PHOTOSPIDER_BUILD_GRAPH_CLI": "ON",
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS": "ON",
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER": "OFF",
        "PHOTOSPIDER_BUILD_IPC": "OFF",
    }
    mismatches = {
        key: (expected_value, values.get(key))
        for key, expected_value in expected.items()
        if values.get(key) != expected_value
    }
    if mismatches:
        raise RuntimeError(
            "nested provider-disabled cache profile mismatch: "
            f"{mismatches}"
        )


def parse_ctest_inventory(
    payload: str,
) -> dict[str, dict[str, object]]:
    """@brief Parse tests and properties from CTest's JSON inventory.

    @param payload Complete stdout from `ctest --show-only=json-v1`.
    @return Mapping from unique registered names to unique properties.
    @throws RuntimeError If the payload is invalid JSON, lacks a test list,
      contains a malformed test/property entry, or repeats a name/property.
    @note The parser consumes CTest's machine-readable schema rather than
      locale-sensitive human inventory text.
    """

    try:
        document = json.loads(payload)
    except json.JSONDecodeError as error:
        raise RuntimeError("CTest inventory is not valid JSON") from error
    tests = document.get("tests") if isinstance(document, dict) else None
    if not isinstance(tests, list):
        raise RuntimeError("CTest inventory has no test list")
    inventory: dict[str, dict[str, object]] = {}
    for test in tests:
        name = test.get("name") if isinstance(test, dict) else None
        if not isinstance(name, str) or not name:
            raise RuntimeError("CTest inventory contains a malformed test")
        if name in inventory:
            raise RuntimeError("CTest inventory contains duplicate test names")
        raw_properties = test.get("properties")
        if not isinstance(raw_properties, list):
            raise RuntimeError("CTest inventory test has no property list")
        properties: dict[str, object] = {}
        for raw_property in raw_properties:
            property_name = (
                raw_property.get("name")
                if isinstance(raw_property, dict)
                else None
            )
            if (
                not isinstance(property_name, str)
                or not property_name
                or "value" not in raw_property
            ):
                raise RuntimeError(
                    "CTest inventory contains a malformed test property"
                )
            if property_name in properties:
                raise RuntimeError(
                    "CTest inventory contains duplicate test properties"
                )
            properties[property_name] = raw_property["value"]
        inventory[name] = properties
    return inventory


def query_ctest_inventory(
    ctest_executable: str,
    build: pathlib.Path,
    configuration: str,
    cwd: pathlib.Path,
) -> dict[str, dict[str, object]]:
    """@brief Query one configured build's real CTest inventory.

    @param ctest_executable CTest executable paired with the selected CMake.
    @param build Configured nested provider-disabled build directory.
    @param configuration Exact build configuration to query.
    @param cwd Existing working directory for the child process.
    @return Registered tests and properties parsed from the JSON-v1 inventory.
    @throws OSError If CTest cannot be started.
    @throws subprocess.CalledProcessError If inventory discovery exits nonzero.
    @throws RuntimeError If the JSON inventory violates its expected schema.
    @note Captured stdout and stderr are echoed before validation so CTest
      retains complete nested-profile diagnostics.
    """

    command = [
        ctest_executable,
        "--test-dir",
        str(build),
        "--show-only=json-v1",
        "-C",
        configuration,
    ]
    print("+", " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.stdout:
        print(completed.stdout, end="", flush=True)
    if completed.stderr:
        print(completed.stderr, end="", flush=True)
    return parse_ctest_inventory(completed.stdout)


def validate_provider_disabled_inventory(
    inventory: dict[str, dict[str, object]],
) -> None:
    """@brief Require the runnable provider-disabled CTest surface.

    @param inventory Unique registered tests and properties from the nested
      build.
    @return None when only the intended focused tests and install smoke exist.
    @throws RuntimeError If a focused test is missing, a broad-suite test
      remains registered, or a disk-cache concurrency property drifts.
    @note `test_kernel_contracts` remains a buildable focused target for the
      separate injected-codec smoke but is deliberately not broadly discovered
      in this provider-disabled CTest inventory.
    """

    disk_cache_tests = {
        (
            "DiskCacheDiagnosticConcurrency."
            "RecordSnapshotClearAndPublicationRemainLive"
        ),
        (
            "DiskCacheDiagnosticConcurrency."
            "SameStoreAndOppositeDirectionExchangeRemainLive"
        ),
        (
            "DiskCacheDiagnosticConcurrency."
            "SnapshotBadAllocReleasesScopedGuard"
        ),
    }
    expected = {
        "DependencyDisabledInstallSmoke",
        (
            "OptionalOpenCvOperationProvider."
            "ReplacementExecutesAndRestores"
        ),
    } | disk_cache_tests
    names = set(inventory)
    if names != expected:
        raise RuntimeError(
            "provider-disabled CTest inventory mismatch: "
            f"expected {sorted(expected)}, got {sorted(names)}"
        )

    property_mismatches = {
        name: {
            "LABELS": inventory[name].get("LABELS"),
            "TIMEOUT": inventory[name].get("TIMEOUT"),
        }
        for name in disk_cache_tests
        if inventory[name].get("LABELS") != ["kernel-concurrency"]
        or inventory[name].get("TIMEOUT") != 20
    }
    if property_mismatches:
        raise RuntimeError(
            "provider-disabled disk-cache CTest property mismatch: "
            f"{property_mismatches}"
        )


def configured_test_executable(
    build: pathlib.Path, configuration: str
) -> pathlib.Path:
    """@brief Resolve the focused test path from nested generator state.

    @param build Configured nested build directory.
    @param configuration Exact configuration requested from the build tool.
    @return Expected focused test executable for the cached generator mode.
    @throws OSError If the nested CMake cache cannot be read.
    @throws UnicodeError If the nested CMake cache is not valid UTF-8 text.
    @throws RuntimeError If configuration metadata is missing or contradicts
      the requested configuration.
    @note A nonempty CMAKE_CONFIGURATION_TYPES value is the sole authority for
      multi-config layout. Platform identity affects only the executable
      suffix and never the presence of a configuration directory.
    """

    cache = cmake_cache_values(build)
    configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if configuration_types:
        available = [
            candidate.strip()
            for candidate in configuration_types.split(";")
            if candidate.strip()
        ]
        if not configuration:
            raise RuntimeError(
                "multi-config nested provider build requires --config"
            )
        if configuration not in available:
            raise RuntimeError(
                "nested provider build configuration mismatch: "
                f"requested {configuration}, available {available}"
            )
        output_directory = build / "tests" / configuration
    else:
        if "CMAKE_BUILD_TYPE" not in cache:
            raise RuntimeError(
                "single-config nested provider build cache is missing "
                "CMAKE_BUILD_TYPE"
            )
        build_type = cache["CMAKE_BUILD_TYPE"]
        if not build_type:
            raise RuntimeError(
                "single-config nested provider build has an empty "
                "CMAKE_BUILD_TYPE"
            )
        if build_type != configuration:
            raise RuntimeError(
                "nested provider build type mismatch: "
                f"requested {configuration}, got {build_type}"
            )
        output_directory = build / "tests"

    executable_suffix = ".exe" if os.name == "nt" else ""
    return output_directory / (
        "test_optional_opencv_operation_provider" + executable_suffix
    )


def main() -> int:
    """@brief Configure, build, and run the provider-disabled regression.

    @return Zero after the focused provider and concurrency cases succeed.
    @throws OSError If path handling or command startup fails.
    @throws SystemExit If command-line parsing rejects the invocation.
    @throws UnicodeError If the nested CMake cache is not valid UTF-8 text.
    @throws ValueError If the destructive work path resolves to a protected
      repository or filesystem location.
    @throws RuntimeError If cleanup, cache metadata, configuration selection,
      or executable discovery violates the nested-build contract.
    @throws subprocess.CalledProcessError If configure, build, or test
      execution exits nonzero.
    @note The function removes only the validated caller-owned work tree before
      configuration. It leaves the successful nested build available to CTest
      cleanup and writes no separate report or provenance artifact.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--cmake-executable", required=True)
    parser.add_argument("--ctest-executable", required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args()

    repo = args.repo.resolve()
    work = remove_work_tree(args.work, repo)
    configuration = args.config or "RelWithDebInfo"

    run(
        [
            args.cmake_executable,
            "-S",
            str(repo),
            "-B",
            str(work),
            f"-DCMAKE_BUILD_TYPE={configuration}",
            "-DBUILD_TESTING=ON",
            "-DPHOTOSPIDER_BUILD_IPC=OFF",
            "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF",
        ],
        repo,
    )
    validate_provider_disabled_cache(cmake_cache_values(work))

    build_command = [
        args.cmake_executable,
        "--build",
        str(work),
        "--target",
        "test_optional_opencv_operation_provider",
        "test_disk_cache_diagnostic_concurrency",
        "-j",
        "4",
    ]
    build_command.extend(["--config", configuration])
    run(build_command, repo)

    executable = configured_test_executable(work, configuration)
    if not executable.is_file():
        raise RuntimeError(
            "nested provider test executable is missing for cached "
            f"configuration: {executable}"
        )
    inventory = query_ctest_inventory(
        args.ctest_executable, work, configuration, repo
    )
    validate_provider_disabled_inventory(inventory)
    run(
        [
            args.ctest_executable,
            "--test-dir",
            str(work),
            "--output-on-failure",
            "-C",
            configuration,
            "-R",
            (
                "^(DiskCacheDiagnosticConcurrency\\..*|"
                "OptionalOpenCvOperationProvider\\."
                "ReplacementExecutesAndRestores)$"
            ),
        ],
        repo,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
