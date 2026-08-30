#!/usr/bin/env python3
"""Verify optional OpenEXR provider package isolation and explicit consumption."""

from __future__ import annotations

import argparse
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from textwrap import dedent

from cmake_build_smoke_support import (
    producer_osx_architecture_arguments,
    remove_work_tree,
    trusted_system_tmp_path_spellings,
)


# Native names whose presence would prove that the optional codec escaped its
# product boundary. Keep these qualified: broad "exr", "deep", "half", or
# "iex" substring scans would create unrelated false positives.
FORBIDDEN_NATIVE_MARKERS = (
    "openexr",
    "openexr_deep_provider",
    "libiex",
    "iex::",
    "iex_",
    "libilmthread",
    "ilmthread::",
    "ilmthread_",
    "libimf",
    "imf::",
    "imf_",
    "libimath",
    "imath::",
    "imath_",
)

# Exact V-14 suite selection keeps the nested BUILD_TESTING profile from ever
# selecting this build-smoke or any other nested smoke.
V14_CTEST_REGEX = r"^VariableSampleFieldExtensions\."


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


def run_capture(command: list[str], cwd: Path, *, echo: bool = False) -> str:
    """@brief Run one command and retain its complete merged output.

    @param command Executable and arguments passed without a shell.
    @param cwd Existing child-process working directory.
    @param echo Whether to emit successful output into the CTest artifact.
    @return Complete stdout/stderr text after a zero exit status.
    @throws OSError If the process cannot start.
    @throws RuntimeError If the process exits nonzero.
    @note Failed output is always included in the raised diagnostic.
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
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited {completed.returncode}: {' '.join(command)}\n"
            + completed.stdout
        )
    if echo and completed.stdout:
        print(completed.stdout, end="", flush=True)
    return completed.stdout


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


def optional_cache_value(build: Path, key: str) -> str | None:
    """@brief Read one optional final CMake cache value.

    @param build Configured producer build directory.
    @param key Exact cache key before its type suffix.
    @return Last serialized value, or None when the key is absent.
    @throws OSError If the cache cannot be read.
    @note Later duplicate assignments win like CMake's effective cache.
    """

    cache = build / "CMakeCache.txt"
    found: str | None = None
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and "=" in line:
            found = line.split("=", 1)[1]
    return found


def assert_openexr_not_discovered(build: Path, configure_trace: str) -> None:
    """@brief Prove the OFF configure executed no OpenEXR package lookup.

    @param build Configured OFF producer build directory.
    @param configure_trace Complete expanded trace for the top-level CMake file.
    @return None after executed-call and cache checks pass.
    @throws OSError If the cache cannot be read.
    @throws RuntimeError If lookup execution or discovery residue is present.
    @note The explicit disable and Photospider option keys are expected facts,
      not discovery results.
    """

    lookup_pattern = re.compile(
        r"\bfind_(?:package|dependency)\s*\(\s*OpenEXR\b", re.IGNORECASE
    )
    executed_lookups = lookup_pattern.findall(configure_trace)
    if executed_lookups:
        raise RuntimeError(
            "OFF producer executed an OpenEXR package lookup:\n" + configure_trace
        )

    allowed_keys = {
        "CMAKE_DISABLE_FIND_PACKAGE_OpenEXR",
        "PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER",
    }
    leaked_keys: list[str] = []
    cache = build / "CMakeCache.txt"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or ":" not in line:
            continue
        key = line.split(":", 1)[0]
        lower_key = key.lower()
        if key in allowed_keys:
            continue
        if any(
            marker in lower_key
            for marker in ("openexr", "ilmthread", "imath", "libiex", "libimf")
        ):
            leaked_keys.append(key)
    if leaked_keys:
        raise RuntimeError(
            f"OFF producer cache contains OpenEXR discovery keys: {leaked_keys}"
        )
    print("OFF producer OpenEXR lookup audit: 0 executed calls, 0 cache keys")


def dependency_surface(path: Path) -> str:
    """@brief Read one native binary's dynamic dependency surface.

    @param path Existing installed library or module.
    @return Native dependency-tool output.
    @throws OSError If a selected inspector cannot start.
    @throws RuntimeError If no supported inspector exists or accepts the file.
    @note Darwin uses otool, Linux uses readelf, and Windows requires dumpbin
      or an objdump implementation that can enumerate PE dependencies.
    """

    if sys.platform == "darwin":
        command = ["otool", "-L", str(path)]
    elif sys.platform.startswith("linux"):
        command = ["readelf", "-d", str(path)]
    elif sys.platform == "win32":
        dumpbin = shutil.which("dumpbin")
        if dumpbin is not None:
            command = [dumpbin, "/dependents", str(path)]
        else:
            objdump = shutil.which("llvm-objdump") or shutil.which("objdump")
            if objdump is None:
                raise RuntimeError(
                    "Windows dependency audit requires dumpbin or objdump"
                )
            command = [objdump, "-p", str(path)]
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
    if not completed.stdout.strip():
        raise RuntimeError(f"dependency inspector returned no data for {path.name}")
    return completed.stdout


def resolve_executable(candidate: str) -> Path | None:
    """@brief Resolve one explicit or PATH-based executable candidate.

    @param candidate Path or command name, possibly empty or NOTFOUND.
    @return Resolved executable path, or None when unusable.
    @throws Nothing for ordinary missing candidates.
    """

    candidate = candidate.strip()
    if not candidate or candidate.endswith("-NOTFOUND"):
        return None
    path = Path(candidate)
    if path.is_file():
        return path.resolve()
    resolved = shutil.which(candidate)
    return Path(resolved).resolve() if resolved is not None else None


def resolve_symbol_tool(requested: str, build: Path) -> tuple[Path, str, str]:
    """@brief Select the producer toolchain's real native symbol inspector.

    @param requested Outer CMake CMAKE_NM value passed by test registration.
    @param build Configured child producer whose CMAKE_NM is authoritative.
    @return Resolved path, tool kind (nm or dumpbin), and provenance label.
    @throws OSError If the child cache cannot be read.
    @throws RuntimeError If no auditable symbol tool is available.
    @note CMake-provided tools precede PATH fallbacks; Windows may use dumpbin.
    """

    child_nm = optional_cache_value(build, "CMAKE_NM") or ""
    candidates = [
        (requested, "outer CMAKE_NM"),
        (child_nm, "child CMAKE_NM"),
        ("llvm-nm", "PATH llvm-nm"),
        ("nm", "PATH nm"),
    ]
    if sys.platform == "win32":
        candidates.append(("dumpbin", "PATH dumpbin"))
    seen: set[Path] = set()
    for candidate, provenance in candidates:
        resolved = resolve_executable(candidate)
        if resolved is None or resolved in seen:
            continue
        seen.add(resolved)
        kind = "dumpbin" if "dumpbin" in resolved.name.lower() else "nm"
        print(f"Native symbol tool: {resolved} ({provenance}, {kind})")
        return resolved, kind, provenance
    raise RuntimeError(
        "OFF symbol audit requires CMAKE_NM, llvm-nm/nm, or Windows dumpbin"
    )


def inspector_output(command: list[str], path: Path, surface: str) -> str:
    """@brief Run one native inspector and require an accepted invocation.

    @param command Complete inspector command.
    @param path Artifact being inspected for diagnostics.
    @param surface Human-readable defined/undefined surface label.
    @return Complete merged inspector output, which may be empty for one class.
    @throws OSError If the inspector cannot start.
    @throws RuntimeError If the inspector rejects the artifact.
    """

    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"symbol inspector rejected {surface} surface of {path}:\n"
            + completed.stdout
        )
    return completed.stdout


def symbol_surfaces(tool: Path, kind: str, path: Path) -> tuple[str, str]:
    """@brief Enumerate global defined and undefined symbols separately.

    @param tool Resolved actual nm or dumpbin executable.
    @param kind Tool family selected by resolve_symbol_tool.
    @param path Runtime/static/product/install artifact to inspect.
    @return Complete defined and undefined symbol texts.
    @throws OSError If the inspector cannot start.
    @throws RuntimeError If the tool rejects the artifact or emits no symbols.
    @note PE DLL/executable surfaces use exports/imports; COFF libraries use
      dumpbin's symbol table with UNDEF classification.
    """

    if kind == "dumpbin":
        if path.suffix.lower() == ".lib":
            table = inspector_output(
                [str(tool), "/symbols", str(path)], path, "COFF symbol"
            )
            external = [line for line in table.splitlines() if "External" in line]
            undefined = [line for line in external if "UNDEF" in line]
            defined = [line for line in external if "UNDEF" not in line]
            defined_text = "\n".join(defined)
            undefined_text = "\n".join(undefined)
        else:
            defined_text = inspector_output(
                [str(tool), "/exports", str(path)], path, "PE export"
            )
            undefined_text = inspector_output(
                [str(tool), "/imports", str(path)], path, "PE import"
            )
    elif sys.platform == "darwin" and tool.name == "nm":
        defined_text = inspector_output(
            [str(tool), "-gU", str(path)], path, "defined"
        )
        undefined_text = inspector_output(
            [str(tool), "-gu", str(path)], path, "undefined"
        )
    else:
        defined_text = inspector_output(
            [str(tool), "-g", "--defined-only", str(path)], path, "defined"
        )
        undefined_text = inspector_output(
            [str(tool), "-g", "--undefined-only", str(path)], path, "undefined"
        )
    if not defined_text.strip() and not undefined_text.strip():
        raise RuntimeError(f"symbol inspector returned no symbols for {path}")
    return defined_text, undefined_text


def native_artifacts(root: Path) -> list[Path]:
    """@brief Enumerate native static/dynamic library artifacts below a root.

    @param root Build output directory or installation prefix.
    @return Sorted unique files with platform library suffixes.
    @throws OSError If recursive enumeration fails.
    @note Test executables and CMake object internals are intentionally outside
      the product/runtime/static artifact roots supplied by the caller.
    """

    if not root.exists():
        return []
    artifacts: list[Path] = []
    for path in root.rglob("*"):
        name = path.name.lower()
        if path.is_file() and (
            path.suffix.lower() in {".a", ".dylib", ".so", ".dll", ".lib"}
            or ".so." in name
        ):
            artifacts.append(path.resolve())
    return sorted(set(artifacts))


def dynamic_artifact(path: Path) -> bool:
    """@brief Classify one native artifact as dynamically loadable.

    @param path Existing product/install artifact.
    @return True for shared libraries, DLLs, and versioned ELF shared objects.
    @throws Nothing.
    """

    name = path.name.lower()
    return path.suffix.lower() in {".dylib", ".so", ".dll", ".exe"} or ".so." in name


def scrub_paths(surface: str, directory_roots: list[Path]) -> str:
    """@brief Remove only known workspace prefixes from auditable tool text.

    @param surface Complete command/dependency/symbol output.
    @param directory_roots Exact work/build/install directory prefixes whose
      names are non-semantic.
    @return Text with each exact native/POSIX directory prefix replaced by an
      audit token while descendant basenames remain visible.
    @throws OSError If trusted Darwin system roots cannot be inspected or
      strictly resolved.
    @throws RuntimeError If trusted root resolution or mapping is invalid.
    @throws ValueError If a supplied directory root is relative or contains
      parent traversal.
    @note Both spellings of Darwin's trusted ``/tmp``/``/private/tmp`` root are
      scrubbed. No caller-controlled symlink is resolved. Leaf artifact and
      evidence-file paths must not be supplied as roots, so header/library
      names below the directory prefixes remain visible and scannable.
    """

    scrubbed = surface
    root_spellings = {
        spelling
        for directory_root in directory_roots
        for spelling in trusted_system_tmp_path_spellings(directory_root)
    }
    for root in sorted(
        root_spellings, key=lambda item: len(str(item)), reverse=True
    ):
        scrubbed = scrubbed.replace(str(root), "<audit-root>")
        scrubbed = scrubbed.replace(root.as_posix(), "<audit-root>")
    return scrubbed


def reject_forbidden_surface(surface: str, context: str) -> None:
    """@brief Reject qualified OpenEXR family markers from one native surface.

    @param surface Scrubbed complete evidence text.
    @param context Artifact/evidence label for diagnostics.
    @return None when no forbidden marker appears.
    @throws RuntimeError With exact matched markers when residue is present.
    """

    lower = surface.lower()
    markers = [marker for marker in FORBIDDEN_NATIVE_MARKERS if marker in lower]
    if markers:
        raise RuntimeError(f"{context} leaked optional codec markers: {markers}")


def validate_native_artifacts(
    artifacts: list[Path],
    tool: Path,
    tool_kind: str,
    roots: list[Path],
    category: str,
) -> None:
    """@brief Audit dependency plus defined/undefined symbol surfaces.

    @param artifacts Exact runtime/static/product/install artifact inventory.
    @param tool Resolved actual symbol inspector.
    @param tool_kind nm or dumpbin selection.
    @param roots Known directory prefixes scrubbed before marker scans.
    @param category Stable evidence label printed into CTest output.
    @return None after every artifact passes.
    @throws OSError If an inspector cannot start.
    @throws RuntimeError For empty inventory, rejected tools, or codec residue.
    @note Artifact leaf paths are deliberately excluded from scrub roots so
      their basenames remain part of the audited symbol/dependency evidence.
    """

    if not artifacts:
        raise RuntimeError(f"{category} native artifact inventory is empty")
    for artifact in artifacts:
        defined, undefined = symbol_surfaces(tool, tool_kind, artifact)
        symbol_text = scrub_paths(defined + "\n" + undefined, roots)
        reject_forbidden_surface(symbol_text, f"{category} symbols for {artifact.name}")
        defined_count = len([line for line in defined.splitlines() if line.strip()])
        undefined_count = len(
            [line for line in undefined.splitlines() if line.strip()]
        )
        print(
            f"SYMBOL-AUDIT {category} {artifact.name}: "
            f"defined={defined_count} undefined={undefined_count}"
        )
        if dynamic_artifact(artifact):
            dependencies = scrub_paths(dependency_surface(artifact), roots)
            reject_forbidden_surface(
                dependencies, f"{category} dependencies for {artifact.name}"
            )
            print(f"DEPENDENCY-AUDIT {category} {artifact.name}: PASS")


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
                project(neutral_sdk_consumer LANGUAGES C)
                find_package(Photospider CONFIG REQUIRED
                  COMPONENTS data_provider_sdk
                  OPTIONAL_COMPONENTS openexr_deep_provider)
                if(Photospider_openexr_deep_provider_FOUND OR
                   TARGET Photospider::openexr_deep_provider)
                  message(FATAL_ERROR "disabled OpenEXR provider was advertised")
                endif()
                set(_neutral_target_surface "")
                foreach(_neutral_target
                    data_provider_sdk operation_plugin_sdk operation_runtime
                    policy_sdk)
                  if(TARGET Photospider::${_neutral_target})
                    foreach(_property
                        INTERFACE_INCLUDE_DIRECTORIES
                        INTERFACE_LINK_LIBRARIES
                        INTERFACE_LINK_OPTIONS
                        INTERFACE_COMPILE_OPTIONS
                        INTERFACE_COMPILE_DEFINITIONS)
                      get_target_property(_value
                        Photospider::${_neutral_target} ${_property})
                      if(_value)
                        string(APPEND _neutral_target_surface
                          "${_neutral_target}.${_property}=${_value}\n")
                      endif()
                    endforeach()
                  endif()
                endforeach()
                file(GENERATE
                  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/neutral-target-$<CONFIG>.txt"
                  CONTENT "${_neutral_target_surface}")
                add_executable(neutral_sdk_consumer main.c)
                target_link_libraries(neutral_sdk_consumer PRIVATE
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
            project(openexr_provider_enabled_consumer LANGUAGES C CXX)
            find_package(Photospider CONFIG REQUIRED
              COMPONENTS data_provider_sdk openexr_deep_provider embedded)
            if(NOT TARGET Photospider::openexr_deep_provider)
              message(FATAL_ERROR "enabled OpenEXR provider target is absent")
            endif()
            if(NOT TARGET Photospider::photospider)
              message(FATAL_ERROR "OpenEXR-enabled embedded target is absent")
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
    required_names = {
        "data_provider_api.h",
        "PhotospiderConfig.cmake",
        "PhotospiderTargets.cmake",
    }
    installed_names = {path.name for path in text_paths}
    missing_names = sorted(required_names - installed_names)
    if missing_names:
        raise RuntimeError(f"OFF install omitted neutral SDK/package files: {missing_names}")

    forbidden_text = (
        "find_dependency(OpenEXR",
        "find_package(OpenEXR",
        "OpenEXR::",
        "<OpenEXR/",
        "<Imath/",
        "Iex::",
        "IlmThread::",
        "Imf::",
        "Imath::",
    )
    for path in text_paths:
        text = path.read_text(encoding="utf-8")
        markers = [marker for marker in forbidden_text if marker in text]
        if markers:
            raise RuntimeError(f"OFF install leaked {markers} through {path}")
    print(
        "OFF install header/Config/Targets audit: "
        f"{len(text_paths)} text surfaces, 0 optional targets/dependencies"
    )


def validate_neutral_target_surface(
    build: Path, config: str, roots: list[Path]
) -> None:
    """@brief Scan evaluated imported-target interfaces from the OFF package.

    @param build Configured and built neutral external-consumer directory.
    @param config Active single- or multi-config name.
    @param roots Exact work/build/install directory prefixes whose names are
      non-semantic.
    @return None after one generated target surface passes.
    @throws OSError If generated evidence cannot be read.
    @throws RuntimeError If evidence is missing or contains codec residue.
    @note The generated evidence-file basename remains visible to the scan;
      only its containing audit directories may be scrubbed.
    """

    preferred = build / f"neutral-target-{config}.txt"
    candidates = [preferred] if preferred.is_file() else sorted(
        build.glob("neutral-target-*.txt")
    )
    if len(candidates) != 1:
        raise RuntimeError(
            f"expected one neutral imported-target surface, found {candidates}"
        )
    surface = candidates[0].read_text(encoding="utf-8")
    if not surface.strip():
        raise RuntimeError("neutral imported-target surface is empty")
    reject_forbidden_surface(
        scrub_paths(surface, roots),
        "neutral imported-target interfaces",
    )
    print(f"TARGET-AUDIT {candidates[0].name}: PASS")


def collect_consumer_command_evidence(build: Path, verbose_output: str) -> str:
    """@brief Collect real external-consumer compile and link command evidence.

    @param build Configured neutral external-consumer build directory.
    @param verbose_output Complete output from its first verbose build.
    @return Concatenated verbose, compile database, link script, and response
      file evidence.
    @throws OSError If an evidence file cannot be read.
    @throws RuntimeError If no real compile or link command can be identified.
    @note Generator-specific files supplement, but never replace, the verbose
      build that actually compiles and links the runnable consumer.
    """

    evidence = [verbose_output]
    evidence_paths: list[Path] = []
    compile_database = build / "compile_commands.json"
    if compile_database.is_file():
        evidence_paths.append(compile_database)
    evidence_paths.extend(sorted(build.rglob("link.txt")))
    evidence_paths.extend(sorted(build.rglob("*.rsp")))
    for path in evidence_paths:
        evidence.append(path.read_text(encoding="utf-8", errors="replace"))
    combined = "\n".join(evidence)
    lower = combined.lower()
    if "main.c" not in lower:
        raise RuntimeError("consumer evidence contains no actual main.c compile")
    if "neutral_sdk_consumer" not in lower:
        raise RuntimeError("consumer evidence contains no actual executable link")
    print(
        "COMMAND-AUDIT neutral consumer: verbose build + "
        f"{len(evidence_paths)} generator evidence files"
    )
    return combined


def run_v14_extension_profile(
    cmake_executable: str, ctest_executable: str, build: Path, config: str
) -> None:
    """@brief Build and run the exact V-14 synthetic extension suite.

    @param cmake_executable Same CMake driver that configured the OFF producer.
    @param ctest_executable CMake-toolchain CTest executable from registration.
    @param build Configured BUILD_TESTING=ON OFF producer directory.
    @param config Active single- or multi-config name.
    @return None after a nonempty exact suite passes.
    @throws OSError If CMake/CTest cannot start.
    @throws RuntimeError If target build, inventory, or tests fail/are empty.
    @note The exact regex plus build-smoke label exclusion is the explicit
      recursion guard; nested package smokes can never match this selection.
    """

    run(
        [
            cmake_executable,
            "--build",
            str(build),
            "--config",
            config,
            "--target",
            "test_variable_sample_field_extensions",
            "--parallel",
            "4",
        ],
        build.parent,
    )
    selection = [
        ctest_executable,
        "-C",
        config,
        "-R",
        V14_CTEST_REGEX,
        "-LE",
        "build-smoke",
    ]
    inventory = run_capture(selection + ["-N"], build, echo=True)
    match = re.search(r"Total Tests:\s*([0-9]+)", inventory)
    if match is None or int(match.group(1)) == 0:
        raise RuntimeError("OFF profile registered no V-14 synthetic tests")
    run(selection + ["--output-on-failure"], build)
    print(
        "V14-AUDIT test_variable_sample_field_extensions: "
        f"{match.group(1)} CTest cases PASS (build-smoke excluded)"
    )


def find_consumer_executable(build: Path, enabled: bool) -> Path:
    """@brief Locate the one freshly built consumer executable.

    @param build Consumer build root.
    @param enabled Whether to find the component-loading or neutral executable.
    @return Exact executable path.
    @throws RuntimeError If zero or multiple candidates remain.
    @note CMakeFiles internals are excluded for single- and multi-config builds.
    """

    stem = "openexr_provider_consumer" if enabled else "neutral_sdk_consumer"
    candidates = [
        path
        for name in (stem, stem + ".exe")
        for path in build.rglob(name)
        if path.is_file() and "CMakeFiles" not in path.parts
    ]
    if len(candidates) != 1:
        raise RuntimeError(f"expected one consumer executable, found {candidates}")
    return candidates[0]


def validate_enabled_provider_path(provider: Path, prefix: Path) -> Path:
    """@brief Validate one enabled provider manifest path below its install.

    @param provider Absolute imported-target path read from the generated
      enabled-consumer manifest.
    @param prefix Physical installed-package prefix owned by this smoke run.
    @return Strictly resolved physical provider path below ``prefix``.
    @throws RuntimeError If metadata cannot be read, the provider is absent or
      outside the prefix, or any untrusted intermediate/leaf symlink follows
      the trusted system tmp root.
    @note Only the shared root-owned Darwin ``/tmp``/``/private/tmp`` mapping
      may change the manifest spelling. No caller-controlled path is resolved
      into the trusted set. The test-owned prefix is not concurrently mutated
      while the component and strict-resolution checks run.
    """

    diagnostic = "enabled imported provider target escaped its prefix"
    try:
        prefix_root = prefix.resolve(strict=True)
        provider_spellings = trusted_system_tmp_path_spellings(provider)
    except (OSError, RuntimeError, ValueError) as error:
        raise RuntimeError(diagnostic) from error
    if not prefix_root.is_dir():
        raise RuntimeError(diagnostic)

    physical_candidates: list[Path] = []
    for spelling in provider_spellings:
        if (
            prefix_root in spelling.parents
            and spelling not in physical_candidates
        ):
            physical_candidates.append(spelling)
    if len(physical_candidates) != 1:
        raise RuntimeError(diagnostic)
    physical_provider = physical_candidates[0]

    try:
        relative_provider = physical_provider.relative_to(prefix_root)
    except ValueError as error:
        raise RuntimeError(diagnostic) from error
    if not relative_provider.parts or ".." in relative_provider.parts:
        raise RuntimeError(diagnostic)

    inspected = prefix_root
    try:
        for component in relative_provider.parts:
            inspected /= component
            if stat.S_ISLNK(inspected.lstat().st_mode):
                raise RuntimeError(diagnostic)
        resolved_provider = physical_provider.resolve(strict=True)
    except OSError as error:
        raise RuntimeError(diagnostic) from error
    try:
        resolved_provider.relative_to(prefix_root)
    except ValueError as error:
        raise RuntimeError(diagnostic) from error
    if (
        resolved_provider != physical_provider
        or not resolved_provider.is_file()
    ):
        raise RuntimeError(diagnostic)
    return resolved_provider


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
    parser.add_argument("--ctest-executable", default="ctest")
    parser.add_argument("--symbol-tool", default="")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--mode", choices=("off", "enabled"), default="off")
    args = parser.parse_args()

    repo = args.repo.resolve(strict=True)
    work = remove_work_tree(args.work, repo)
    build = work / "producer-build"
    prefix = work / "prefix"
    consumer_source = work / "neutral-consumer-source"
    consumer_build = work / "neutral-consumer-build"
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
        configure = [args.cmake_executable]
        if not enabled:
            configure.extend(
                [
                    "--trace-expand",
                    f"--trace-source={repo / 'CMakeLists.txt'}",
                ]
            )
        configure.extend(
            [
                "-S",
                str(repo),
                "-B",
                str(build),
                f"-DCMAKE_BUILD_TYPE={args.config}",
                f"-DCMAKE_INSTALL_PREFIX={prefix}",
                f"-DBUILD_TESTING={'OFF' if enabled else 'ON'}",
                "-DPHOTOSPIDER_ENABLE_OPENCV=OFF",
                "-DPHOTOSPIDER_ENABLE_YAML=OFF",
                "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF",
                "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS=OFF",
                "-DPHOTOSPIDER_BUILD_GRAPH_CLI=OFF",
                f"-DPHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER={'ON' if enabled else 'OFF'}",
                "-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=ON",
                "-DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=ON",
                *outer_architecture,
            ]
        )
        if not enabled:
            configure.append("-DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON")
        configure_trace = run_capture(configure, repo)
        expected_cache = "ON" if enabled else "OFF"
        if cache_value(build, "PHOTOSPIDER_BUILD_OPENEXR_DEEP_PROVIDER") != expected_cache:
            raise RuntimeError("producer serialized the wrong OpenEXR option state")
        expected_testing = "OFF" if enabled else "ON"
        if cache_value(build, "BUILD_TESTING") != expected_testing:
            raise RuntimeError("producer serialized the wrong BUILD_TESTING state")
        if not enabled:
            assert_openexr_not_discovered(build, configure_trace)
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
        if not enabled:
            run_v14_extension_profile(
                args.cmake_executable,
                args.ctest_executable,
                build,
                args.config,
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
            symbol_tool, symbol_kind, _ = resolve_symbol_tool(
                args.symbol_tool, build
            )
            build_artifacts: list[Path] = []
            for artifact_root in (build / "lib", build / "bin", build / "plugins"):
                build_artifacts.extend(native_artifacts(artifact_root))
            build_artifacts = sorted(set(build_artifacts))
            install_artifacts = native_artifacts(prefix)
            audit_roots = [work, build, prefix]
            validate_native_artifacts(
                build_artifacts,
                symbol_tool,
                symbol_kind,
                audit_roots,
                "OFF-build-product",
            )
            validate_native_artifacts(
                install_artifacts,
                symbol_tool,
                symbol_kind,
                audit_roots,
                "OFF-install-product",
            )
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
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            *producer_osx_architecture_arguments(build),
        ]
        if not enabled:
            consumer_configure.append("-DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON")
        run(consumer_configure, repo)
        consumer_build_output = run_capture(
            [
                args.cmake_executable,
                "--build",
                str(consumer_build),
                "--config",
                args.config,
                "--parallel",
                "4",
                "--verbose",
            ],
            repo,
            echo=not enabled,
        )
        executable = find_consumer_executable(consumer_build, enabled)
        if enabled:
            manifest = consumer_build / f"provider-{args.config}.txt"
            provider = validate_enabled_provider_path(
                Path(manifest.read_text(encoding="utf-8")), prefix
            )
            run([str(executable), str(provider)], work)
        else:
            command_evidence = collect_consumer_command_evidence(
                consumer_build, consumer_build_output
            )
            consumer_audit_roots = [
                work,
                build,
                prefix,
                consumer_source,
                consumer_build,
            ]
            scrubbed_commands = scrub_paths(
                command_evidence, consumer_audit_roots
            )
            reject_forbidden_surface(
                scrubbed_commands, "neutral consumer compile/link commands"
            )
            validate_neutral_target_surface(
                consumer_build, args.config, consumer_audit_roots
            )
            validate_native_artifacts(
                [executable],
                symbol_tool,
                symbol_kind,
                consumer_audit_roots,
                "OFF-neutral-consumer",
            )
            executable_dependencies = scrub_paths(
                dependency_surface(executable),
                consumer_audit_roots,
            )
            reject_forbidden_surface(
                executable_dependencies, "neutral consumer dependencies"
            )
            print("DEPENDENCY-AUDIT OFF-neutral-consumer executable: PASS")
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
