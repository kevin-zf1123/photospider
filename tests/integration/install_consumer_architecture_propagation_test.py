#!/usr/bin/env python3
"""Regression tests for install-consumer CMake architecture propagation."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

import cmake_build_smoke_support as architecture_support
import dependency_disabled_install_smoke as dependency_disabled
import ipc_disabled_install_smoke as ipc_disabled
import static_product_consumer_smoke as static_product


ARCHITECTURES = "arm64;x86_64"
ARCHITECTURE_ARGUMENT = f"-DCMAKE_OSX_ARCHITECTURES={ARCHITECTURES}"


def write_cmake_cache(
    build: pathlib.Path, values: dict[str, str]
) -> None:
    """@brief Write a real CMake cache-shaped producer fixture.

    @param build Synthetic producer build directory to create.
    @param values Exact cache key/value assignments to serialize.
    @return None.
    @throws OSError If the directory or cache file cannot be written.
    @note Every value is written as one CMake ``STRING`` cache assignment.
      Tests use the production cache readers rather than an in-memory stub.
    """

    build.mkdir(parents=True)
    (build / "CMakeCache.txt").write_text(
        "".join(f"{key}:STRING={value}\n" for key, value in values.items()),
        encoding="utf-8",
    )


def base_producer_cache(
    repo: pathlib.Path, build: pathlib.Path
) -> dict[str, str]:
    """@brief Build common identity, configuration, and architecture values.

    @param repo Resolved synthetic source directory cached by the producer.
    @param build Resolved synthetic producer build directory.
    @return Mutable cache mapping for one single-config producer fixture.
    @throws None This helper performs no filesystem or process I/O.
    @note The multi-architecture value intentionally contains a semicolon so
      the regression proves it remains one subprocess argv element.
    """

    return {
        "CMAKE_HOME_DIRECTORY": str(repo),
        "CMAKE_CACHEFILE_DIR": str(build),
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "CMAKE_CONFIGURATION_TYPES": "",
        "CMAKE_OSX_ARCHITECTURES": ARCHITECTURES,
        "BUILD_TESTING": "OFF",
        "PHOTOSPIDER_BUILD_IPC": "OFF",
    }


class CommandRecorder:
    """@brief Record driver commands and synthesize consumer executables.

    @throws OSError If a requested synthetic executable cannot be created.
    @note The callable never starts a process. Configure argv still comes from
      each production driver's real ``main`` control flow.
    """

    def __init__(
        self, executable_by_build: dict[pathlib.Path, str]
    ) -> None:
        """@brief Initialize one command recorder.

        @param executable_by_build Consumer build directories mapped to the
          executable basename their driver later discovers.
        @return None.
        @throws None Initialization only copies the caller-owned mapping.
        @note Paths are resolved before storage so driver path spelling cannot
          affect the synthetic build lookup.
        """

        self.commands: list[list[str]] = []
        self.expected_failure_commands: list[list[str]] = []
        self._executable_by_build = {
            path.resolve(): name
            for path, name in executable_by_build.items()
        }

    def run(self, command: list[str], cwd: pathlib.Path) -> int:
        """@brief Record one successful driver command.

        @param command Executable and argv produced by the smoke driver.
        @param cwd Working directory selected by the smoke driver.
        @return Zero, matching a successful required subprocess.
        @throws OSError If a synthetic consumer executable cannot be written.
        @note ``cwd`` is intentionally not inspected. A consumer build command
          creates only the empty file that the driver subsequently discovers;
          no command is executed.
        """

        del cwd
        recorded = list(command)
        self.commands.append(recorded)
        if len(recorded) >= 3 and recorded[1] == "--build":
            build = pathlib.Path(recorded[2]).resolve()
            executable_name = self._executable_by_build.get(build)
            if executable_name:
                build.mkdir(parents=True, exist_ok=True)
                (build / executable_name).write_text("", encoding="utf-8")
        return 0

    def expect_failure(
        self,
        command: list[str],
        cwd: pathlib.Path,
        *expected_diagnostic: str,
    ) -> None:
        """@brief Record one configure command expected to fail.

        @param command Executable and argv produced by the smoke driver.
        @param cwd Working directory selected by the smoke driver.
        @param expected_diagnostic Optional stable diagnostic accepted by the
          dependency-disabled driver's callback shape.
        @return None.
        @throws None The test replaces the real failing subprocess.
        @note Both install-consumer drivers share this recorder despite their
          two- and three-argument failure callback signatures.
        """

        del cwd, expected_diagnostic
        self.expected_failure_commands.append(list(command))

    def configure_commands(self) -> list[list[str]]:
        """@brief Return every recorded child CMake configure argv.

        @return Successful and expected-failure commands whose first argument
          after the executable is ``-S``.
        @throws None The returned lists are detached copies.
        @note Producer configure is absent because tests reuse a validated
          synthetic producer cache.
        """

        return [
            list(command)
            for command in (
                self.commands + self.expected_failure_commands
            )
            if len(command) >= 2 and command[1] == "-S"
        ]


class ProducerArchitectureArgumentPolicyTest(unittest.TestCase):
    """@brief Verify the shared cache-to-argv platform policy.

    @throws AssertionError If cache parsing, false-value handling, platform
      gating, or multi-architecture argv preservation differs.
    @note Tests read real temporary CMakeCache.txt files and never start CMake.
    """

    def test_preserves_single_and_multi_architecture_values(self) -> None:
        """@brief Preserve exact Darwin architecture values as one argv item.

        @return None after both cache values produce their expected tuple.
        @throws OSError If a synthetic cache cannot be written.
        @throws AssertionError If either value is changed, split, or omitted.
        @note The multi-architecture case proves semicolons are subprocess data,
          not shell or Python list separators.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-architecture-policy-values-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            for index, architectures in enumerate(
                ("arm64", "arm64;x86_64")
            ):
                with self.subTest(architectures=architectures):
                    build = sandbox / f"build-{index}"
                    write_cmake_cache(
                        build,
                        {
                            "CMAKE_OSX_ARCHITECTURES": architectures,
                        },
                    )
                    arguments = (
                        architecture_support.producer_osx_architecture_arguments(
                            build, system_name="Darwin"
                        )
                    )
                    self.assertEqual(
                        arguments,
                        (f"-DCMAKE_OSX_ARCHITECTURES={architectures}",),
                    )

    def test_ignores_absent_empty_and_cmake_false_values(self) -> None:
        """@brief Reject cache values that do not name an architecture.

        @return None after every absent, empty, false, and NOTFOUND case is a
          no-op.
        @throws OSError If a synthetic cache cannot be written.
        @throws AssertionError If any meaningless value becomes child argv.
        @note ``ARCH-NOTFOUND`` covers CMake's suffixed NOTFOUND convention.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-architecture-policy-empty-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            missing_build = sandbox / "missing"
            self.assertEqual(
                architecture_support.producer_osx_architecture_arguments(
                    missing_build, system_name="Darwin"
                ),
                (),
            )
            for index, architectures in enumerate(
                ("", "OFF", "FALSE", "NOTFOUND", "ARCH-NOTFOUND")
            ):
                with self.subTest(architectures=architectures):
                    build = sandbox / f"build-{index}"
                    write_cmake_cache(
                        build,
                        {
                            "CMAKE_OSX_ARCHITECTURES": architectures,
                        },
                    )
                    arguments = (
                        architecture_support.producer_osx_architecture_arguments(
                            build, system_name="Darwin"
                        )
                    )
                    self.assertEqual(
                        arguments,
                        (),
                    )

    def test_does_not_forward_macos_cache_values_on_linux(self) -> None:
        """@brief Keep the macOS-only CMake argument out of Linux children.

        @return None after a nonempty producer value produces no Linux argv.
        @throws OSError If the synthetic cache cannot be written.
        @throws AssertionError If platform gating leaks the option.
        @note The cache intentionally contains a valid Darwin value, proving
          platform gating is independent of string truthiness.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-architecture-policy-linux-"
        ) as temporary:
            build = pathlib.Path(temporary) / "build"
            write_cmake_cache(
                build,
                {
                    "CMAKE_OSX_ARCHITECTURES": "arm64",
                },
            )
            self.assertEqual(
                architecture_support.producer_osx_architecture_arguments(
                    build, system_name="Linux"
                ),
                (),
            )


class InstallConsumerArchitecturePropagationTest(unittest.TestCase):
    """@brief Verify all install-consumer drivers inherit producer architecture.

    @throws AssertionError If a driver omits, duplicates, or splits the
      producer's resolved macOS architecture argument.
    @note All files live below disposable synthetic roots. CMake, compilers,
      linkers, and installed executables are never launched.
    """

    def assert_propagated(
        self, commands: list[list[str]], expected_count: int
    ) -> None:
        """@brief Require one exact architecture argv element on every command.

        @param commands Child configure argv captured from a production driver.
        @param expected_count Exact driver-specific child configure count.
        @return None after all commands contain one unsplit architecture value.
        @throws AssertionError If the count or any architecture argument differs.
        @note The semicolon-bearing multi-architecture list must remain one
          list element because production subprocess calls do not use a shell.
        """

        self.assertEqual(len(commands), expected_count)
        for command in commands:
            with self.subTest(command=command):
                self.assertEqual(
                    [
                        argument
                        for argument in command
                        if argument.startswith(
                            "-DCMAKE_OSX_ARCHITECTURES="
                        )
                    ],
                    [ARCHITECTURE_ARGUMENT],
                )

    def test_dependency_disabled_driver_propagates_to_all_children(
        self,
    ) -> None:
        """@brief Cover component, invalid-profile, and real consumer configures.

        @return None after all six child configure commands inherit the cache.
        @throws OSError If a synthetic fixture cannot be created.
        @throws AssertionError If the driver rejects its producer or omits an
          architecture argument.
        @note The test exercises the real reusable-producer validator and
          ``main`` command construction while replacing subprocess execution.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-dependency-architecture-"
        ) as temporary:
            sandbox = pathlib.Path(temporary).resolve()
            repo = sandbox / "repo"
            producer = sandbox / "producer"
            work = sandbox / "work"
            repo.mkdir()
            cache = base_producer_cache(repo, producer)
            cache.update(
                {
                    "PHOTOSPIDER_ENABLE_OPENCV": "OFF",
                    "PHOTOSPIDER_ENABLE_YAML": "OFF",
                    "PHOTOSPIDER_BUILD_GRAPH_CLI": "OFF",
                    "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER": "OFF",
                    "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS": "OFF",
                    "CMAKE_DISABLE_FIND_PACKAGE_OpenCV": "ON",
                    "CMAKE_DISABLE_FIND_PACKAGE_yaml-cpp": "ON",
                }
            )
            write_cmake_cache(producer, cache)
            recorder = CommandRecorder(
                {
                    work / "consumer-build": (
                        "dependency_disabled_consumer"
                    )
                }
            )
            argv = [
                "dependency_disabled_install_smoke.py",
                "--repo",
                str(repo),
                "--work",
                str(work),
                "--producer-build",
                str(producer),
                "--config",
                "RelWithDebInfo",
            ]

            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(
                    dependency_disabled, "run", side_effect=recorder.run
                ),
                mock.patch.object(
                    dependency_disabled,
                    "run_expect_failure",
                    side_effect=recorder.expect_failure,
                ),
                mock.patch(
                    "platform.system", return_value="Darwin"
                ),
            ):
                self.assertEqual(dependency_disabled.main(), 0)

            self.assert_propagated(
                recorder.configure_commands(), expected_count=6
            )

    def test_ipc_disabled_driver_propagates_to_all_children(self) -> None:
        """@brief Cover optional, required-missing, and embedded consumers.

        @return None after all three child configure commands inherit the cache.
        @throws OSError If a synthetic fixture cannot be created.
        @throws AssertionError If the driver rejects its producer or omits an
          architecture argument.
        @note The real install/package command construction runs with only
          subprocess execution replaced by the recorder.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-ipc-architecture-"
        ) as temporary:
            sandbox = pathlib.Path(temporary).resolve()
            repo = sandbox / "repo"
            producer = sandbox / "producer"
            work = sandbox / "work"
            repo.mkdir()
            write_cmake_cache(
                producer, base_producer_cache(repo, producer)
            )
            recorder = CommandRecorder(
                {
                    work / "consumer-build": "ipc_disabled_consumer",
                }
            )
            argv = [
                "ipc_disabled_install_smoke.py",
                "--repo",
                str(repo),
                "--work",
                str(work),
                "--producer-build",
                str(producer),
                "--config",
                "RelWithDebInfo",
            ]

            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(
                    ipc_disabled, "run", side_effect=recorder.run
                ),
                mock.patch.object(
                    ipc_disabled,
                    "run_expect_failure",
                    side_effect=recorder.expect_failure,
                ),
                mock.patch(
                    "platform.system", return_value="Darwin"
                ),
            ):
                self.assertEqual(ipc_disabled.main(), 0)

            self.assert_propagated(
                recorder.configure_commands(), expected_count=3
            )

    def test_static_product_driver_propagates_to_all_children(self) -> None:
        """@brief Cover all package, SDK, missing, and unknown consumers.

        @return None after all eight child configure commands inherit the cache.
        @throws OSError If a synthetic fixture cannot be created.
        @throws AssertionError If command construction omits any architecture.
        @note Expensive product behavior is replaced after the real driver has
          consumed its cache and constructed every child configure command.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-static-product-architecture-"
        ) as temporary:
            sandbox = pathlib.Path(temporary).resolve()
            repo = sandbox / "repo"
            producer = sandbox / "producer"
            work = sandbox / "work"
            repo.mkdir()
            cache = base_producer_cache(repo, producer)
            cache.update(
                {
                    "OpenCV_DIR": str(sandbox / "opencv"),
                    "CMAKE_GENERATOR": "Ninja",
                    "CMAKE_INSTALL_LIBDIR": "lib",
                    "PHOTOSPIDER_INSTALL_CMAKEDIR": (
                        "lib/cmake/Photospider"
                    ),
                }
            )
            write_cmake_cache(producer, cache)
            recorder = CommandRecorder({})
            argv = [
                "static_product_consumer_smoke.py",
                "--repo",
                str(repo),
                "--build",
                str(producer),
                "--work",
                str(work),
                "--config",
                "RelWithDebInfo",
            ]
            empty_discovery = {"selected": ""}

            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(
                    static_product,
                    "write_consumer_projects",
                    return_value=([], {"client": [], "host": []}),
                ),
                mock.patch.object(
                    static_product,
                    "write_extension_consumer_projects",
                ),
                mock.patch.object(
                    static_product,
                    "write_missing_opencv_component_projects",
                ),
                mock.patch.object(
                    static_product,
                    "run_command",
                    side_effect=recorder.run,
                ),
                mock.patch.object(
                    static_product,
                    "find_consumer_executable",
                    return_value=empty_discovery,
                ),
                mock.patch.object(
                    static_product,
                    "inspect_install_tree",
                    return_value={},
                ),
                mock.patch.object(
                    static_product, "evaluate_behavior", return_value=True
                ),
                mock.patch(
                    "platform.system", return_value="Darwin"
                ),
            ):
                self.assertEqual(static_product.main(), 0)

            self.assert_propagated(
                recorder.configure_commands(), expected_count=8
            )


if __name__ == "__main__":
    unittest.main()
