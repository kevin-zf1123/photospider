#!/usr/bin/env python3
"""Verify optional OpenEXR provider package isolation and explicit consumption."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path
from textwrap import dedent

from cmake_build_smoke_support import (
    producer_osx_architecture_arguments,
    remove_work_tree,
)


def run(command: list[str], cwd: Path) -> None:
    """@brief Run one required smoke command with inherited output.

    @param command Executable and arguments passed without a shell.
    @param cwd Existing child-process working directory.
    @return None after a zero exit status.
    @throws OSError If the process cannot start.
    @throws subprocess.CalledProcessError If the process exits nonzero.
    @note Commands are printed so CTest artifacts remain independently useful.
    """

    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def run_expect_failure(
    command: list[str], cwd: Path, required_diagnostic: str
) -> None:
    """@brief Require one configure to fail with an owned component message.

    @param command Executable and arguments passed without a shell.
    @param cwd Existing child-process working directory.
    @param required_diagnostic Exact diagnostic fragment that must be present.
    @return None after the expected bounded failure.
    @throws OSError If the process cannot start.
    @throws RuntimeError If it succeeds or reports a different failure.
    @note Captured text is emitted only when validation fails.
    """

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode == 0:
        raise RuntimeError("required absent component unexpectedly configured")
    if required_diagnostic not in completed.stdout:
        raise RuntimeError(
            "required absent component failed without the owned diagnostic:\n"
            + completed.stdout
        )


def cache_value(build: Path, key: str) -> str:
    """@brief Read one exact final CMake cache value.

    @param build Configured producer build directory.
    @param key Exact cache key before its type suffix.
    @return Last serialized value for the key.
    @throws OSError If the cache cannot be read.
    @throws RuntimeError If the key is absent.
    @note Later duplicate assignments win like CMake's effective cache.
    """

    cache = build / "CMakeCache.txt"
    found: str | None = None
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and "=" in line:
            found = line.split("=", 1)[1]
    if found is None:
        raise RuntimeError(f"producer cache is missing {key}")
    return found


def dependency_surface(path: Path) -> str:
    """@brief Read one native binary's dynamic dependency surface.

    @param path Existing installed library or module.
    @return Native dependency-tool output.
    @throws OSError If a selected inspector cannot start.
    @throws RuntimeError If no supported inspector exists or accepts the file.
    @note Darwin uses otool and Linux uses readelf; Windows package loading is
      validated by the enabled consumer instead of a platform-specific dump.
    """

    if sys.platform == "darwin":
        command = ["otool", "-L", str(path)]
    elif sys.platform.startswith("linux"):
        command = ["readelf", "-d", str(path)]
    elif sys.platform == "win32":
        return ""
    else:
        raise RuntimeError("unsupported native dependency-inspection platform")
    if shutil.which(command[0]) is None:
        raise RuntimeError(f"required dependency inspector is absent: {command[0]}")
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"dependency inspector rejected {path.name}")
    return completed.stdout


def write_consumer(source: Path, enabled: bool) -> None:
    """@brief Write one independent package consumer for the selected profile.

    @param source Fresh consumer source directory.
    @param enabled Whether the installed provider component must be present.
    @return None after writing CMake and pure-C ABI sources.
    @throws OSError If source creation or writes fail.
    @note The enabled executable loads only the two frozen v3 exports; the OFF
      executable proves a neutral SDK consumer configures with OpenEXR disabled.
    """

    source.mkdir(parents=True)
    if not enabled:
        (source / "CMakeLists.txt").write_text(
            dedent(
                """\
                cmake_minimum_required(VERSION 3.16)
                project(openexr_provider_off_consumer LANGUAGES C)
                find_package(Photospider CONFIG REQUIRED
                  COMPONENTS data_provider_sdk
                  OPTIONAL_COMPONENTS openexr_deep_provider)
                if(Photospider_openexr_deep_provider_FOUND OR
                   TARGET Photospider::openexr_deep_provider)
                  message(FATAL_ERROR "disabled OpenEXR provider was advertised")
                endif()
                add_executable(openexr_provider_consumer main.c)
                target_link_libraries(openexr_provider_consumer PRIVATE
                  Photospider::data_provider_sdk)
                """
            ),
            encoding="utf-8",
        )
        (source / "main.c").write_text(
            dedent(
                """\
                #include <photospider/plugin/data_provider_api.h>

                /** @brief Verify the installed dependency-neutral ABI header. */
                int main(void) {
                  return PS_DATA_PROVIDER_ABI_VERSION == 3U ? 0 : 1;
                }
                """
            ),
            encoding="utf-8",
        )
        return

    (source / "CMakeLists.txt").write_text(
        dedent(
            """\
            cmake_minimum_required(VERSION 3.16)
            project(openexr_provider_enabled_consumer LANGUAGES C)
            find_package(Photospider CONFIG REQUIRED
              COMPONENTS data_provider_sdk openexr_deep_provider)
            if(NOT TARGET Photospider::openexr_deep_provider)
              message(FATAL_ERROR "enabled OpenEXR provider target is absent")
            endif()
            file(GENERATE
              OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/provider-$<CONFIG>.txt"
              CONTENT "$<TARGET_FILE:Photospider::openexr_deep_provider>")
            add_executable(openexr_provider_consumer main.c)
            target_link_libraries(openexr_provider_consumer PRIVATE
              Photospider::data_provider_sdk ${CMAKE_DL_LIBS})
            """
        ),
        encoding="utf-8",
    )
    (source / "main.c").write_text(
        dedent(
            """\
            #include <photospider/plugin/data_provider_api.h>

            #include <stdint.h>
            #include <stdio.h>

            #if defined(_WIN32)
            #include <windows.h>
            typedef HMODULE module_handle;
            /** @brief Open one installed provider DSO. */
            static module_handle open_module(const char* path) {
              return LoadLibraryA(path);
            }
            /** @brief Resolve one exact provider export. */
            static void* find_symbol(module_handle module, const char* name) {
              return (void*)GetProcAddress(module, name);
            }
            /** @brief Close one installed provider DSO. */
            static void close_module(module_handle module) { FreeLibrary(module); }
            #else
            #include <dlfcn.h>
            typedef void* module_handle;
            /** @brief Open one installed provider DSO. */
            static module_handle open_module(const char* path) {
              return dlopen(path, RTLD_NOW | RTLD_LOCAL);
            }
            /** @brief Resolve one exact provider export. */
            static void* find_symbol(module_handle module, const char* name) {
              return dlsym(module, name);
            }
            /** @brief Close one installed provider DSO. */
            static void close_module(module_handle module) { (void)dlclose(module); }
            #endif

            /** @brief Load and exercise the exact installed v3 provider table. */
            int main(int argc, char** argv) {
              module_handle module;
              union {
                void* object;
                ps_data_provider_get_abi_version_fn_v3 function;
              } abi_symbol;
              union {
                void* object;
                ps_data_provider_get_api_fn_v3 function;
              } api_symbol;
              ps_data_provider_api_v3 api = {0};
              ps_data_diagnostic_v3 diagnostic = {0};
              if (argc != 2) return 2;
              module = open_module(argv[1]);
              if (module == NULL) return 3;
              abi_symbol.object = find_symbol(
                  module, "ps_data_provider_get_abi_version");
              api_symbol.object = find_symbol(module, "ps_data_provider_get_api_v3");
              if (abi_symbol.object == NULL || api_symbol.object == NULL ||
                  abi_symbol.function() != PS_DATA_PROVIDER_ABI_VERSION) {
                close_module(module);
                return 4;
              }
              api.struct_size = PS_DATA_PROVIDER_API_V3_SIZE;
              if (api_symbol.function(&api) != PS_DATA_STATUS_OK_V3 ||
                  api.abi_version != PS_DATA_PROVIDER_ABI_VERSION ||
                  api.definition_count != 4U || api.destroy_provider == NULL) {
                close_module(module);
                return 5;
              }
              diagnostic.struct_size = PS_DATA_DIAGNOSTIC_V3_SIZE;
              if (api.destroy_provider(api.provider_context, &diagnostic, NULL) !=
                  PS_DATA_STATUS_OK_V3) {
                close_module(module);
                return 6;
              }
              close_module(module);
              return 0;
            }
            """
        ),
        encoding="utf-8",
    )


def write_required_absent_probe(source: Path) -> None:
    """@brief Write one configure-only required-component negative probe.

    @param source Fresh probe source directory.
    @return None after writing its CMake project.
    @throws OSError If directory creation or the write fails.
    @note OpenEXR discovery is disabled by the caller, so the package must
      diagnose its own absent component instead of reaching the dependency.
    """

    source.mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(
        dedent(
            """\
            cmake_minimum_required(VERSION 3.16)
            project(openexr_provider_required_absent LANGUAGES C)
            find_package(Photospider CONFIG REQUIRED
              COMPONENTS openexr_deep_provider)
            """
        ),
        encoding="utf-8",
    )


def write_default_package_probe(source: Path) -> None:
    """@brief Write a no-component neutral package-discovery probe.

    @param source Fresh probe source directory.
    @return None after writing its CMake project.
    @throws OSError If directory creation or the write fails.
    @note The caller disables OpenEXR discovery, so a leaked default lookup is
      a deterministic configure failure in addition to the target assertions.
    """

    source.mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(
        dedent(
            """\
            cmake_minimum_required(VERSION 3.16)
            project(openexr_provider_default_neutral LANGUAGES C)
            find_package(Photospider CONFIG REQUIRED)
            if(Photospider_openexr_deep_provider_FOUND OR
               TARGET Photospider::openexr_deep_provider)
              message(FATAL_ERROR "default package request advertised OpenEXR")
            endif()
            """
        ),
        encoding="utf-8",
    )


def validate_disabled_install(prefix: Path) -> None:
    """@brief Reject optional codec residue from one OFF installation.

    @param prefix Fresh installation root.
    @return None after package, header, artifact, and binary surfaces pass.
    @throws OSError If installed surfaces cannot be read or inspected.
    @throws RuntimeError If any OpenEXR dependency or provider artifact leaks.
    @note Component-name diagnostics remain legal; dependency namespace and
      linker tokens are the bounded forbidden surface.
    """

    leaked_artifacts = [
        path
        for path in prefix.rglob("*")
        if path.is_file()
        and (
            "PhotospiderOpenEXRTargets" in path.name
            or "openexr_deep_provider" in path.name.lower()
        )
    ]
    if leaked_artifacts:
        raise RuntimeError(f"OFF install leaked provider artifacts: {leaked_artifacts}")

    text_paths = [
        path
        for path in prefix.rglob("*")
        if path.is_file()
        and (
            "include/photospider/" in path.as_posix()
            or path.suffix in {".cmake", ".pc"}
        )
    ]
    forbidden_text = ("find_dependency(OpenEXR", "OpenEXR::", "<OpenEXR/", "Imf::")
    for path in text_paths:
        text = path.read_text(encoding="utf-8")
        markers = [marker for marker in forbidden_text if marker in text]
        if markers:
            raise RuntimeError(f"OFF install leaked {markers} through {path}")

    library_suffixes = (".a", ".dylib", ".so", ".dll", ".lib")
    for path in prefix.rglob("*"):
        lower_name = path.name.lower()
        if not path.is_file() or not (
            lower_name.endswith(library_suffixes) or ".so." in lower_name
        ):
            continue
        surface = dependency_surface(path).lower().replace(str(path).lower(), "")
        if any(marker in surface for marker in ("openexr", "libiex", "libilmthread")):
            raise RuntimeError(f"OFF installed binary links optional codec: {path}")


def find_consumer_executable(build: Path) -> Path:
    """@brief Locate the one freshly built consumer executable.

    @param build Consumer build root.
    @return Exact executable path.
    @throws RuntimeError If zero or multiple candidates remain.
    @note CMakeFiles internals are excluded for single- and multi-config builds.
    """

    candidates = [
        path
        for name in ("openexr_provider_consumer", "openexr_provider_consumer.exe")
        for path in build.rglob(name)
        if path.is_file() and "CMakeFiles" not in path.parts
    ]
    if len(candidates) != 1:
        raise RuntimeError(f"expected one consumer executable, found {candidates}")
    return candidates[0]


def main() -> int:
    """@brief Run one disposable OFF-isolation or enabled-consumer profile.

    @return Zero after all configure/build/install/consumer assertions pass.
    @throws Exception On any failed process or semantic assertion.
    @note The validated work tree is always removed, including after failure.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--producer-build", required=True, type=Path)
    parser.add_argument("--cmake-executable", default="cmake")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--mode", choices=("off", "enabled"), default="off")
    args = parser.parse_args()

    repo = args.repo.resolve(strict=True)
    work = remove_work_tree(args.work, repo)
    build = work / "producer-build"
    prefix = work / "prefix"
    consumer_source = work / "consumer-source"
    consumer_build = work / "consumer-build"
    absent_source = work / "absent-source"
    absent_build = work / "absent-build"
    default_source = work / "default-source"
    default_build = work / "default-build"
    outer_architecture = producer_osx_architecture_arguments(
        args.producer_build.resolve(strict=True)
    )
    enabled = args.mode == "enabled"

    try:
        work.mkdir(parents=True)
        configure = [
            args.cmake_executable,
            "-S",
            str(repo),
            "-B",
            str(build),
            f"-DCMAKE_BUILD_TYPE={args.config}",
            f"-DCMAKE_INSTALL_PREFIX={prefix}",
            "-DBUILD_TESTING=OFF",
            "-DPHOTOSPIDER_ENABLE_OPENCV=OFF",
            "-DPHOTOSPIDER_ENABLE_YAML=OFF",
            "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF",
            "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=OFF",
            "-DPHOTOSPIDER_BUILD_GRAPH_CLI=OFF",
            "-DPHOTOSPIDER_BUILD_IPC=OFF",
            f"-DPHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER={'ON' if enabled else 'OFF'}",
            "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
            "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
            *outer_architecture,
        ]
        if not enabled:
            configure.append("-DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON")
        run(configure, repo)
        expected_cache = "ON" if enabled else "OFF"
        if cache_value(build, "PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER") != expected_cache:
            raise RuntimeError("producer serialized the wrong OpenEXR option state")
        run(
            [
                args.cmake_executable,
                "--build",
                str(build),
                "--config",
                args.config,
                "--parallel",
                "4",
            ],
            repo,
        )
        run(
            [
                args.cmake_executable,
                "--install",
                str(build),
                "--config",
                args.config,
            ],
            repo,
        )

        if not enabled:
            validate_disabled_install(prefix)
            write_default_package_probe(default_source)
            run(
                [
                    args.cmake_executable,
                    "-S",
                    str(default_source),
                    "-B",
                    str(default_build),
                    f"-DCMAKE_PREFIX_PATH={prefix}",
                    "-DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON",
                    *producer_osx_architecture_arguments(build),
                ],
                repo,
            )
            write_required_absent_probe(absent_source)
            run_expect_failure(
                [
                    args.cmake_executable,
                    "-S",
                    str(absent_source),
                    "-B",
                    str(absent_build),
                    f"-DCMAKE_PREFIX_PATH={prefix}",
                    "-DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON",
                    *producer_osx_architecture_arguments(build),
                ],
                repo,
                "Photospider was built without the openexr_deep_provider component",
            )

        write_consumer(consumer_source, enabled)
        consumer_configure = [
            args.cmake_executable,
            "-S",
            str(consumer_source),
            "-B",
            str(consumer_build),
            f"-DCMAKE_BUILD_TYPE={args.config}",
            f"-DCMAKE_PREFIX_PATH={prefix}",
            *producer_osx_architecture_arguments(build),
        ]
        if not enabled:
            consumer_configure.append("-DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON")
        run(consumer_configure, repo)
        run(
            [
                args.cmake_executable,
                "--build",
                str(consumer_build),
                "--config",
                args.config,
                "--parallel",
                "4",
            ],
            repo,
        )
        executable = find_consumer_executable(consumer_build)
        if enabled:
            manifest = consumer_build / f"provider-{args.config}.txt"
            provider = Path(manifest.read_text(encoding="utf-8"))
            if not provider.is_file() or prefix not in provider.parents:
                raise RuntimeError("enabled imported provider target escaped its prefix")
            run([str(executable), str(provider)], work)
        else:
            run([str(executable)], work)
        print(f"OpenEXR deep provider {args.mode} package smoke: PASS", flush=True)
        return 0
    finally:
        remove_work_tree(work, repo)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - CTest needs one diagnostic.
        print(f"OpenEXR deep provider package smoke: FAIL: {error}", file=sys.stderr)
        raise
