#!/usr/bin/env python3
"""Regression tests for install-consumer platform policy and registration."""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from typing import Any
from unittest import mock

import cmake_build_smoke_support as architecture_support
import dependency_disabled_install_smoke as dependency_disabled
import ipc_disabled_install_smoke as ipc_disabled
import static_product_consumer_smoke as static_product


#: @brief Resolved repository root shared by inventory and driver assertions.
#: @note The path is immutable for the test-process lifetime and is never used
#:   as a cleanup target.
REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
#: @brief Exact CI inventory implementation loaded as the system under test.
#: @note Loading by path keeps direct script execution independent of cwd and
#:   does not install or copy the production module.
BUILD_SMOKE_INVENTORY_MODULE_PATH = (
    REPOSITORY_ROOT / "ci" / "scripts" / "build_smoke_inventory.py"
)
#: @brief Unique process-local module identity for dataclass initialization.
#: @note The name avoids colliding with other unittest modules that may load the
#:   same production script under their own isolated identity.
BUILD_SMOKE_INVENTORY_MODULE_NAME = (
    "_photospider_install_consumer_build_smoke_inventory"
)
#: @brief Import specification for the production CTest inventory parser.
#: @note The loader releases its source file after synchronous module execution.
BUILD_SMOKE_INVENTORY_SPEC = importlib.util.spec_from_file_location(
    BUILD_SMOKE_INVENTORY_MODULE_NAME,
    BUILD_SMOKE_INVENTORY_MODULE_PATH,
)
if (
    BUILD_SMOKE_INVENTORY_SPEC is None
    or BUILD_SMOKE_INVENTORY_SPEC.loader is None
):
    raise RuntimeError(
        f"Cannot load {BUILD_SMOKE_INVENTORY_MODULE_PATH}"
    )
#: @brief Process-local production inventory module used by all registrations.
#: @note sys.modules registration is required by dataclass initialization and
#:   lasts only until this test process exits.
ctest_inventory = importlib.util.module_from_spec(
    BUILD_SMOKE_INVENTORY_SPEC
)
sys.modules[BUILD_SMOKE_INVENTORY_MODULE_NAME] = ctest_inventory
BUILD_SMOKE_INVENTORY_SPEC.loader.exec_module(ctest_inventory)


#: @brief Module-owned multi-architecture CMake cache fixture for all cases.
#: @note The semicolon remains part of one value for the test-process lifetime;
#:   tests treat this fixture as read-only subprocess argv data.
ARCHITECTURES = "arm64;x86_64"
#: @brief Exact child CMake argument derived from the architecture fixture.
#: @note This process-lifetime assertion value must stay aligned with
#:   ``ARCHITECTURES`` and remain one unsplit argv element.
ARCHITECTURE_ARGUMENT = f"-DCMAKE_OSX_ARCHITECTURES={ARCHITECTURES}"
#: @brief Minimal usable symbol table covering every production seam object.
#: @note Each line uses a defined text-symbol marker accepted by the production
#:   archive scanner and contains no forbidden test-seam fragment.
USABLE_PRODUCT_SYMBOL_OUTPUT = "\n".join(
    f"0000000000000000 T {fragment}"
    for fragment in static_product.REQUIRED_PRODUCT_SEAM_SYMBOL_FRAGMENTS
)
#: @brief Exact maintained install-consumer CTest names and driver basenames.
#: @note Every expected entry carries the build-smoke label in real CTest
#:   inventory; ordering is stable only for deterministic diagnostics.
INSTALL_CONSUMER_CTEST_REGISTRATIONS = (
    (
        "DependencyDisabledInstallSmoke",
        "dependency_disabled_install_smoke.py",
    ),
    (
        "IpcDisabledInstallSmoke",
        "ipc_disabled_install_smoke.py",
    ),
    (
        "StaticProductConsumerSmoke",
        "static_product_consumer_smoke.py",
    ),
)
#: @brief Exact maintained names derived from the command contract table.
#: @note CMake passes a configuration-specific subset because the static
#:   product smoke exists only when IPC product support is enabled.
INSTALL_CONSUMER_CTEST_NAMES = tuple(
    test_name
    for test_name, _driver_name in INSTALL_CONSUMER_CTEST_REGISTRATIONS
)


def ctest_test_entry(
    name: str,
    command: list[str] | None,
    *,
    disabled: bool = False,
    labels: tuple[str, ...] = ("build-smoke",),
) -> dict[str, Any]:
    """@brief Create one detached synthetic CTest json-v1 test entry.

    @param name Exact CTest test name.
    @param command Optional command argv; None omits executable state.
    @param disabled Whether to add the boolean CTest DISABLED property.
    @param labels Exact ordered label values serialized for the entry.
    @return Newly owned JSON-compatible test-entry mapping.
    @throws None This helper performs no filesystem or subprocess I/O.
    @note Synthetic command and property containers are copied so mutations in
      one fail-closed subtest cannot affect another.
    """

    properties: list[dict[str, Any]] = [
        {"name": "LABELS", "value": list(labels)}
    ]
    if disabled:
        properties.append({"name": "DISABLED", "value": True})
    entry: dict[str, Any] = {
        "name": name,
        "properties": properties,
    }
    if command is not None:
        entry["command"] = list(command)
    return entry


def ctest_inventory_payload(
    entries: list[dict[str, Any]],
) -> bytes:
    """@brief Serialize one complete synthetic CTest json-v1 inventory.

    @param entries Test entries in their intended numeric CTest index order.
    @return UTF-8 ctestInfo payload accepted by the production parser.
    @throws TypeError If an entry is not JSON serializable.
    @throws UnicodeError If serialized text cannot be encoded as UTF-8.
    @note Serialization is in-memory and retains no caller-owned containers.
    """

    return json.dumps(
        {
            "kind": "ctestInfo",
            "tests": entries,
            "version": {"major": 1, "minor": 0},
        }
    ).encode("utf-8")


def expected_install_consumer_ctest_entries(
    python_executable: str,
    expected_test_names: tuple[str, ...] = INSTALL_CONSUMER_CTEST_NAMES,
) -> list[dict[str, Any]]:
    """@brief Build valid entries for one maintained configuration subset.

    @param python_executable Exact launcher expected at command argv index zero.
    @param expected_test_names Configuration-specific exact maintained subset.
    @return Newly owned entries with the exact ``python -B driver`` prefixes.
    @throws ctest_inventory.InventoryError If a requested name is unknown,
      duplicated, or the expected subset is empty.
    @note Remaining driver arguments are intentionally absent because this
      fixture validates the registration prefix rather than driver behavior.
    """

    if not expected_test_names:
        raise ctest_inventory.InventoryError(
            "At least one install-consumer smoke must be expected."
        )
    if len(set(expected_test_names)) != len(expected_test_names):
        raise ctest_inventory.InventoryError(
            "Expected install-consumer smoke names must be unique."
        )
    driver_by_name = dict(INSTALL_CONSUMER_CTEST_REGISTRATIONS)
    unknown_names = tuple(
        name for name in expected_test_names if name not in driver_by_name
    )
    if unknown_names:
        raise ctest_inventory.InventoryError(
            "Unknown expected install-consumer smoke names: "
            f"{list(unknown_names)!r}."
        )
    integration_directory = REPOSITORY_ROOT / "tests" / "integration"
    return [
        ctest_test_entry(
            test_name,
            [
                python_executable,
                "-B",
                str(integration_directory / driver_by_name[test_name]),
            ],
        )
        for test_name in expected_test_names
    ]


def require_install_consumer_ctest_commands(
    inventory: Any,
    *,
    python_executable: str,
    expected_test_names: tuple[str, ...] = INSTALL_CONSUMER_CTEST_NAMES,
) -> None:
    """@brief Validate the configuration-specific install-smoke command set.

    The check requires each expected exact name once and every other maintained
    name zero times. It then reuses the production build-smoke selector to
    reject missing labels, disabled state, and unusable commands. Each
    surviving argv must start with the configured Python launcher, immediately
    followed by ``-B`` and the exact maintained driver.

    @param inventory Parsed production ``Inventory`` instance to validate.
    @param python_executable Exact CMake-selected Python launcher.
    @param expected_test_names Exact maintained subset enabled by this CMake
      configuration.
    @return None after the exact set and every live command prefix pass.
    @throws ctest_inventory.InventoryError If expected names are unknown,
      duplicated, or empty; an expected name is absent/duplicated; a
      configuration-inactive maintained name is present; or an expected entry
      lacks the label, is disabled/commandless, or has a different command
      prefix.
    @note The function inspects only immutable in-memory inventory. It neither
      launches CTest nor executes any smoke command.
    """

    expected_install_consumer_ctest_entries(
        python_executable,
        expected_test_names,
    )
    expected_name_set = set(expected_test_names)
    integration_directory = REPOSITORY_ROOT / "tests" / "integration"
    for test_name, driver_name in INSTALL_CONSUMER_CTEST_REGISTRATIONS:
        matches = tuple(
            test for test in inventory.tests if test.name == test_name
        )
        expected_count = 1 if test_name in expected_name_set else 0
        if len(matches) != expected_count:
            expected_state = (
                "appear exactly once"
                if expected_count == 1
                else "be absent"
            )
            raise ctest_inventory.InventoryError(
                f"Maintained install-consumer smoke {test_name!r} must "
                f"{expected_state} in CTest inventory for this configuration; "
                f"observed {len(matches)} entries."
            )
        if expected_count == 0:
            continue
        record = inventory.require_selected(test_name)
        expected_prefix = (
            python_executable,
            "-B",
            str(integration_directory / driver_name),
        )
        if record.command is None or record.command[:3] != expected_prefix:
            observed_prefix = (
                None
                if record.command is None
                else list(record.command[:3])
            )
            raise ctest_inventory.InventoryError(
                f"Install-consumer smoke {test_name!r} must start with "
                f"{list(expected_prefix)!r}; observed {observed_prefix!r}."
            )


def parse_live_inventory_arguments(
    argv: list[str],
) -> tuple[argparse.Namespace, list[str]]:
    """@brief Separate optional live-CTest inputs from unittest arguments.

    @param argv Complete process argv including the script path.
    @return Parsed live options plus argv preserved for ``unittest.main``.
    @throws SystemExit If argparse rejects a live option or build-dir use omits
      the configured Python launcher or expected smoke set.
    @note Direct no-option execution leaves live validation disabled. CTest
      supplies all live inputs, while standard unittest flags pass through.
    """

    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--ctest-executable", default="ctest")
    parser.add_argument("--config")
    parser.add_argument("--python-executable")
    parser.add_argument(
        "--expected-smoke",
        action="append",
        choices=INSTALL_CONSUMER_CTEST_NAMES,
        default=[],
    )
    options, unittest_arguments = parser.parse_known_args(argv[1:])
    if options.build_dir is not None and not options.python_executable:
        parser.error(
            "--python-executable is required when --build-dir enables "
            "live CTest validation"
        )
    if options.build_dir is not None and not options.expected_smoke:
        parser.error(
            "at least one --expected-smoke is required when --build-dir "
            "enables live CTest validation"
        )
    return options, [argv[0], *unittest_arguments]


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

        #: @brief Successful command argv snapshots owned by this recorder.
        #: @note Detached lists retain call order for one test-local recorder
        #:   lifetime and are never shared back with production drivers.
        self.commands: list[list[str]] = []
        #: @brief Expected-failure argv snapshots owned by this recorder.
        #: @note Detached lists live for one test-local recorder lifetime and
        #:   are merged with successful snapshots only by
        #:   ``configure_commands``.
        self.expected_failure_commands: list[list[str]] = []
        #: @brief Private resolved build-to-executable fixture map.
        #: @note The recorder owns this detached mapping for its lifetime;
        #:   ``run`` only reads it when synthesizing disposable executable
        #:   files.
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


class SymbolToolHarness:
    """@brief Provide deterministic executable discovery and scan processes.

    @throws AssertionError If production invokes an unconfigured executable.
    @note The harness records only process-local lists, never launches a child,
      and never changes PATH or another process-global environment value.
    """

    def __init__(
        self,
        which_paths: dict[str, str | None],
        process_results: dict[str, tuple[int, str] | OSError],
        executable_paths: set[str] | None = None,
    ) -> None:
        """@brief Initialize one isolated symbol-tool fixture.

        @param which_paths Executable names mapped to synthetic lookup results.
        @param process_results Executable paths mapped to return-code/stdout
          pairs or startup failures.
        @param executable_paths Paths accepted from ``xcrun --find``; omitted
          values default to an empty set.
        @return None.
        @throws None Caller containers are copied before retention.
        @note Stderr is intentionally absent because production diagnostics
          must not echo arbitrary tool output.
        """

        #: @brief Ordered executable names requested through ``which``.
        #: @note The list is owned for one test-local harness lifetime.
        self.which_calls: list[str] = []
        #: @brief Ordered no-shell command argv observed by the fake runner.
        #: @note Detached lists allow exact priority assertions after a scan.
        self.run_commands: list[list[str]] = []
        #: @brief Synthetic executable lookup table owned by this harness.
        #: @note Missing names model unavailable PATH candidates.
        self._which_paths = dict(which_paths)
        #: @brief Synthetic captured-process outcomes keyed by executable path.
        #: @note Values never leave this test-process fixture.
        self._process_results = dict(process_results)
        #: @brief Exact xcrun result paths accepted by the fake validator.
        #: @note Membership is immutable after initialization.
        self._executable_paths = set(executable_paths or set())

    def which(self, executable_name: str) -> str | None:
        """@brief Resolve one synthetic executable name.

        @param executable_name Basename requested by production discovery.
        @return Configured path or ``None`` when unavailable.
        @throws None Missing mapping entries are treated as unavailable.
        @note Every lookup is recorded in call order.
        """

        self.which_calls.append(executable_name)
        return self._which_paths.get(executable_name)

    def run(
        self, command: list[str], cwd: pathlib.Path
    ) -> subprocess.CompletedProcess[str]:
        """@brief Return one configured captured-process result.

        @param command Executable and arguments produced by production code.
        @param cwd Working directory selected by production code.
        @return Synthetic completed process with configured status/stdout.
        @throws OSError When the configured process models startup failure.
        @throws AssertionError If no response exists for the executable.
        @note ``cwd`` is not accessed; argv is copied before recording.
        """

        del cwd
        recorded = list(command)
        self.run_commands.append(recorded)
        if not recorded or recorded[0] not in self._process_results:
            raise AssertionError(
                f"unexpected symbol-tool command: {recorded!r}"
            )
        result = self._process_results[recorded[0]]
        if isinstance(result, OSError):
            raise result
        returncode, stdout = result
        return subprocess.CompletedProcess(
            args=recorded,
            returncode=returncode,
            stdout=stdout,
            stderr="fixture stderr must remain private",
        )

    def is_executable(self, path: str) -> bool:
        """@brief Validate one synthetic ``xcrun`` result path.

        @param path Exact discovery text after production whitespace trimming.
        @return Whether the path belongs to the configured executable set.
        @throws None Membership lookup is in-memory.
        @note The production helper independently enforces absolute spelling.
        """

        return path in self._executable_paths

    def run_executables(self) -> list[str]:
        """@brief Return executable paths in observed process order.

        @return Detached first-argv values from every recorded command.
        @throws None Recorded commands are always nonempty after ``run``.
        @note Discovery and inspection commands remain distinguishable by path.
        """

        return [command[0] for command in self.run_commands]


class InstallConsumerCTestRegistrationTest(unittest.TestCase):
    """@brief Lock active CTest commands for the three real install smokes.

    @throws OSError If an explicitly configured live CTest query cannot start.
    @throws ctest_inventory.InventoryError If live or synthetic inventory is
      malformed or violates the exact registration contract.
    @throws AssertionError If a fail-closed counterexample is accepted.
    @note Synthetic cases perform no process I/O. The live case runs only when
      CMake passes a configured build tree; it queries inventory without
      executing a configure, build, install, or smoke command.
    """

    #: @brief Optional configured build tree used only by the live test.
    #: @note None keeps direct Python execution independent of a producer tree.
    live_build_dir: pathlib.Path | None = None
    #: @brief CTest launcher paired with ``live_build_dir``.
    #: @note CMake overrides the process-local default with
    #:   ``CMAKE_CTEST_COMMAND``.
    live_ctest_executable: str = "ctest"
    #: @brief Optional multi-configuration selector for live discovery.
    #: @note None or an empty string omits ``-C`` in the production query.
    live_config: str | None = None
    #: @brief Exact configured Python launcher required by live commands.
    #: @note None is valid only while live validation is disabled.
    live_python_executable: str | None = None
    #: @brief Exact maintained smoke subset enabled by the live configuration.
    #: @note CMake supplies two names when IPC is disabled and three when
    #:   enabled; direct no-option execution leaves the tuple empty.
    live_expected_test_names: tuple[str, ...] = ()

    def test_valid_synthetic_inventory_matches_all_commands(self) -> None:
        """@brief Accept both legal configuration-specific active sets.

        @return None after IPC-enabled and IPC-disabled command sets pass.
        @throws AssertionError If the valid synthetic inventory is rejected.
        @note The case exercises the production parser and semantic checker
          entirely in memory.
        """

        python_executable = "/fixture/python3"
        profiles = (
            ("ipc-enabled", INSTALL_CONSUMER_CTEST_NAMES),
            ("ipc-disabled", INSTALL_CONSUMER_CTEST_NAMES[:2]),
        )
        for profile, expected_test_names in profiles:
            with self.subTest(profile=profile):
                inventory = ctest_inventory.parse_inventory(
                    ctest_inventory_payload(
                        expected_install_consumer_ctest_entries(
                            python_executable,
                            expected_test_names,
                        )
                    )
                )
                require_install_consumer_ctest_commands(
                    inventory,
                    python_executable=python_executable,
                    expected_test_names=expected_test_names,
                )

        unexpected_static_inventory = ctest_inventory.parse_inventory(
            ctest_inventory_payload(
                expected_install_consumer_ctest_entries(
                    python_executable,
                    INSTALL_CONSUMER_CTEST_NAMES,
                )
            )
        )
        with self.assertRaisesRegex(
            ctest_inventory.InventoryError,
            "must be absent",
        ):
            require_install_consumer_ctest_commands(
                unexpected_static_inventory,
                python_executable=python_executable,
                expected_test_names=INSTALL_CONSUMER_CTEST_NAMES[:2],
            )

    def test_missing_inactive_and_commented_entries_fail_closed(
        self,
    ) -> None:
        """@brief Reject every source state that yields no active CTest entry.

        @return None after missing, ``if(FALSE)``, and bracket-comment models
          each fail because one exact required name is absent.
        @throws AssertionError If any omitted registration is accepted.
        @note CTest JSON contains neither inactive branches nor comments, so
          each counterexample is correctly represented by omitting that entry
          rather than parsing CMake source text.
        """

        python_executable = "/fixture/python3"
        source_states = (
            ("missing", 0),
            ("inactive-if-false", 1),
            ("bracket-commented", 2),
        )
        for source_state, missing_index in source_states:
            with self.subTest(source_state=source_state):
                entries = expected_install_consumer_ctest_entries(
                    python_executable
                )
                entries.pop(missing_index)
                inventory = ctest_inventory.parse_inventory(
                    ctest_inventory_payload(entries)
                )
                with self.assertRaisesRegex(
                    ctest_inventory.InventoryError,
                    "must appear exactly once",
                ):
                    require_install_consumer_ctest_commands(
                        inventory,
                        python_executable=python_executable,
                    )

    def test_duplicate_disabled_and_commandless_entries_fail_closed(
        self,
    ) -> None:
        """@brief Reject non-unique or non-executable maintained registrations.

        @return None after duplicate, disabled, and commandless counterexamples
          are rejected by production inventory validation.
        @throws AssertionError If identity or executable-state drift is
          accepted.
        @note Every case starts from newly allocated valid entries and performs
          no filesystem or process I/O.
        """

        python_executable = "/fixture/python3"
        duplicate_entries = expected_install_consumer_ctest_entries(
            python_executable
        )
        duplicate_entries.append(
            ctest_test_entry(
                duplicate_entries[0]["name"],
                list(duplicate_entries[0]["command"]),
            )
        )
        with self.assertRaisesRegex(
            ctest_inventory.InventoryError,
            "duplicated",
        ):
            ctest_inventory.parse_inventory(
                ctest_inventory_payload(duplicate_entries)
            )

        for state in ("disabled", "commandless"):
            with self.subTest(state=state):
                entries = expected_install_consumer_ctest_entries(
                    python_executable
                )
                command = list(entries[0]["command"])
                entries[0] = ctest_test_entry(
                    str(entries[0]["name"]),
                    None if state == "commandless" else command,
                    disabled=state == "disabled",
                )
                inventory = ctest_inventory.parse_inventory(
                    ctest_inventory_payload(entries)
                )
                diagnostic = (
                    "disabled"
                    if state == "disabled"
                    else "no CTest command"
                )
                with self.assertRaisesRegex(
                    ctest_inventory.InventoryError,
                    diagnostic,
                ):
                    require_install_consumer_ctest_commands(
                        inventory,
                        python_executable=python_executable,
                    )

    def test_launcher_flag_and_driver_drift_fail_closed(self) -> None:
        """@brief Reject every semantic command-prefix drift independently.

        @return None after wrong launcher, displaced ``-B``, and wrong driver
          each fail the exact argv-prefix contract.
        @throws AssertionError If a syntactically valid but incorrect command
          prefix is accepted.
        @note The production JSON parser intentionally accepts generic command
          argv; this integration-specific checker owns their semantics.
        """

        python_executable = "/fixture/python3"
        command_mutations = (
            ("wrong-launcher", 0, "/fixture/not-python3"),
            ("missing-bytecode-flag", 1, "--version"),
            (
                "wrong-driver",
                2,
                str(
                    REPOSITORY_ROOT
                    / "tests"
                    / "integration"
                    / "not_the_install_driver.py"
                ),
            ),
        )
        for state, argument_index, replacement in command_mutations:
            with self.subTest(state=state):
                entries = expected_install_consumer_ctest_entries(
                    python_executable
                )
                command = list(entries[0]["command"])
                command[argument_index] = replacement
                entries[0]["command"] = command
                inventory = ctest_inventory.parse_inventory(
                    ctest_inventory_payload(entries)
                )
                with self.assertRaisesRegex(
                    ctest_inventory.InventoryError,
                    "must start with",
                ):
                    require_install_consumer_ctest_commands(
                        inventory,
                        python_executable=python_executable,
                    )

    def test_configured_ctest_inventory_matches_all_commands(self) -> None:
        """@brief Validate commands emitted by the current configured tree.

        @return None after a live ``ctest --show-only=json-v1`` query and all
          exact registration checks pass, or after an explicit direct-run skip.
        @throws OSError If the configured CTest process cannot be started.
        @throws ctest_inventory.InventoryError If discovery or registration
          validation fails.
        @note Direct Python execution deliberately skips only this live case;
          CMake supplies the authoritative tree, executable, config, and
          launcher when the safety regression runs through CTest.
        """

        if self.live_build_dir is None:
            self.skipTest(
                "live CTest validation requires CMake --build-dir metadata"
            )
        if self.live_python_executable is None:
            self.fail(
                "live CTest validation has no configured Python launcher"
            )
        inventory = ctest_inventory.query_ctest(
            self.live_ctest_executable,
            self.live_build_dir,
            self.live_config,
        )
        require_install_consumer_ctest_commands(
            inventory,
            python_executable=self.live_python_executable,
            expected_test_names=self.live_expected_test_names,
        )


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


class ProductArchiveSymbolInspectionPolicyTest(unittest.TestCase):
    """@brief Lock platform symbol-tool ordering and fail-closed fallback.

    @throws AssertionError If discovery order, validation, de-duplication,
      archive usability checks, or path-free diagnostics regress.
    @note Every case injects callbacks into the real production helpers. No
      process launches and no process-global PATH mutation occur.
    """

    def inspect(
        self,
        harness: SymbolToolHarness,
        platform_system: str = "Darwin",
    ) -> dict[str, Any]:
        """@brief Inspect one synthetic installed archive through production.

        @param harness Test-local lookup, validation, and process callbacks.
        @param platform_system Platform policy selected by the production code.
        @return Complete production symbol-scan observation.
        @throws AssertionError If production invokes an unconfigured command.
        @note The synthetic archive need not exist because the injected runner
          never accesses its argv path.
        """

        return static_product.inspect_product_archive_symbols(
            pathlib.Path("/fixture/install-prefix"),
            ["lib/libphotospider.a"],
            platform_system,
            which=harness.which,
            captured_runner=harness.run,
            executable_validator=harness.is_executable,
        )

    def test_darwin_uses_xcrun_before_incompatible_path_llvm_nm(
        self,
    ) -> None:
        """@brief Prefer Xcode llvm-nm even when PATH has another llvm-nm.

        @return None after only xcrun discovery and its usable result execute.
        @throws AssertionError If PATH-first or xcrun-skipping behavior returns.
        @note The PATH llvm-nm intentionally lacks every required anchor, so a
          priority mutation cannot accidentally satisfy the assertion.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/fake/path/llvm-nm",
                "nm": "/usr/bin/nm",
            },
            {
                "/usr/bin/xcrun": (0, "/xcode/usr/bin/llvm-nm\n"),
                "/xcode/usr/bin/llvm-nm": (
                    0,
                    USABLE_PRODUCT_SYMBOL_OUTPUT,
                ),
                "/fake/path/llvm-nm": (0, "0000 T unrelated"),
                "/usr/bin/nm": (0, "0000 T unrelated"),
            },
            {"/xcode/usr/bin/llvm-nm"},
        )

        observation = self.inspect(harness)

        self.assertEqual(observation["tool_source"], "xcrun llvm-nm")
        self.assertTrue(observation["covers_product_seams"])
        self.assertEqual(
            harness.run_executables(),
            ["/usr/bin/xcrun", "/xcode/usr/bin/llvm-nm"],
        )
        self.assertEqual(harness.which_calls.count("xcrun"), 1)

    def test_darwin_xcrun_failure_falls_back_to_path_llvm_nm(
        self,
    ) -> None:
        """@brief Continue to PATH llvm-nm after xcrun discovery fails.

        @return None after the second candidate supplies the authoritative scan.
        @throws AssertionError If discovery failure skips or aborts fallback.
        @note PATH nm exists but must not execute after PATH llvm-nm succeeds.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/toolchain/llvm-nm",
                "nm": "/usr/bin/nm",
            },
            {
                "/usr/bin/xcrun": (69, "private discovery output"),
                "/toolchain/llvm-nm": (0, USABLE_PRODUCT_SYMBOL_OUTPUT),
                "/usr/bin/nm": (0, USABLE_PRODUCT_SYMBOL_OUTPUT),
            },
        )

        observation = self.inspect(harness)

        self.assertEqual(observation["tool_source"], "PATH llvm-nm")
        self.assertEqual(
            harness.run_executables(),
            ["/usr/bin/xcrun", "/toolchain/llvm-nm"],
        )
        self.assertIn(
            "xcrun llvm-nm: discovery exited with status 69",
            observation["attempts"],
        )

    def test_darwin_invalid_xcrun_path_falls_back_to_path_llvm_nm(
        self,
    ) -> None:
        """@brief Reject an unusable absolute xcrun result before scanning.

        @return None after validated PATH llvm-nm completes the scan.
        @throws AssertionError If unvalidated xcrun stdout becomes executable.
        @note The fake validator rejects the Xcode path without filesystem I/O.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/toolchain/llvm-nm",
                "nm": None,
            },
            {
                "/usr/bin/xcrun": (0, "/xcode/missing/llvm-nm\n"),
                "/toolchain/llvm-nm": (0, USABLE_PRODUCT_SYMBOL_OUTPUT),
            },
        )

        observation = self.inspect(harness)

        self.assertEqual(observation["tool_source"], "PATH llvm-nm")
        self.assertEqual(
            harness.run_executables(),
            ["/usr/bin/xcrun", "/toolchain/llvm-nm"],
        )
        self.assertIn(
            "xcrun llvm-nm: discovery returned an unusable path",
            observation["attempts"],
        )

    def test_darwin_unusable_path_llvm_nm_falls_back_to_nm(self) -> None:
        """@brief Try PATH nm when earlier discovery/scan candidates fail.

        @return None after nm supplies all three required anchors.
        @throws AssertionError If a zero-exit but anchor-blind tool is accepted.
        @note This models a PATH llvm-nm that cannot inspect the installed
          archive format despite exiting successfully.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/toolchain/llvm-nm",
                "nm": "/usr/bin/nm",
            },
            {
                "/usr/bin/xcrun": (1, ""),
                "/toolchain/llvm-nm": (0, "0000 T unrelated"),
                "/usr/bin/nm": (0, USABLE_PRODUCT_SYMBOL_OUTPUT),
            },
        )

        observation = self.inspect(harness)

        self.assertEqual(observation["tool_source"], "PATH nm")
        self.assertEqual(
            harness.run_executables(),
            ["/usr/bin/xcrun", "/toolchain/llvm-nm", "/usr/bin/nm"],
        )
        self.assertIn(
            "PATH llvm-nm: inspection missed 3/3 required anchors",
            observation["attempts"],
        )

    def test_non_darwin_never_discovers_or_invokes_xcrun(self) -> None:
        """@brief Keep xcrun entirely outside non-Darwin resolution.

        @return None after PATH llvm-nm is selected without an xcrun lookup.
        @throws AssertionError If non-Darwin behavior depends on Xcode tooling.
        @note An available xcrun mapping makes an accidental lookup observable.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/toolchain/llvm-nm",
                "nm": "/usr/bin/nm",
            },
            {
                "/usr/bin/xcrun": (0, "/xcode/usr/bin/llvm-nm\n"),
                "/toolchain/llvm-nm": (0, USABLE_PRODUCT_SYMBOL_OUTPUT),
                "/usr/bin/nm": (0, USABLE_PRODUCT_SYMBOL_OUTPUT),
            },
            {"/xcode/usr/bin/llvm-nm"},
        )

        observation = self.inspect(harness, platform_system="Linux")

        self.assertEqual(observation["tool_source"], "PATH llvm-nm")
        self.assertEqual(harness.which_calls, ["llvm-nm", "nm"])
        self.assertEqual(harness.run_executables(), ["/toolchain/llvm-nm"])

    def test_no_symbol_tool_fails_closed(self) -> None:
        """@brief Reject an archive scan when every discovery branch is absent.

        @return None after the unsuccessful observation names all safe sources.
        @throws AssertionError If missing tools become a skip or successful scan.
        @note No fake process is configured, proving no command can execute.
        """

        harness = SymbolToolHarness(
            {"xcrun": None, "llvm-nm": None, "nm": None},
            {},
        )

        observation = self.inspect(harness)

        self.assertEqual(observation["tool"], "")
        self.assertIsNone(observation["status"])
        self.assertFalse(observation["covers_product_seams"])
        self.assertEqual(harness.run_commands, [])
        self.assertIn("xcrun llvm-nm: xcrun is unavailable", observation["stderr"])
        self.assertIn("PATH llvm-nm: executable is unavailable", observation["stderr"])
        self.assertIn("PATH nm: executable is unavailable", observation["stderr"])

    def test_all_unusable_candidates_fail_with_path_free_reasons(self) -> None:
        """@brief Reject startup, nonzero, and missing-anchor candidates.

        @return None after every attempted source and reason appears safely.
        @throws AssertionError If any unusable candidate passes or a private
          executable/captured-stderr path reaches the failure summary.
        @note The three independent failure modes exercise the complete loop.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/private/toolchain/llvm-nm",
                "nm": "/private/toolchain/nm",
            },
            {
                "/usr/bin/xcrun": (0, "/private/xcode/llvm-nm\n"),
                "/private/xcode/llvm-nm": OSError(
                    "private startup diagnostic"
                ),
                "/private/toolchain/llvm-nm": (2, "private failure output"),
                "/private/toolchain/nm": (0, "0000 T unrelated"),
            },
            {"/private/xcode/llvm-nm"},
        )

        observation = self.inspect(harness)

        self.assertEqual(observation["tool"], "")
        self.assertFalse(observation["covers_product_seams"])
        self.assertIn(
            "xcrun llvm-nm: inspection could not start",
            observation["stderr"],
        )
        self.assertIn(
            "PATH llvm-nm: inspection exited with status 2",
            observation["stderr"],
        )
        self.assertIn(
            "PATH nm: inspection missed 3/3 required anchors",
            observation["stderr"],
        )
        self.assertNotIn("/private/", observation["stderr"])
        self.assertNotIn("private failure output", observation["stderr"])

    def test_duplicate_canonical_candidate_paths_run_once(self) -> None:
        """@brief De-duplicate xcrun, llvm-nm, and nm path aliases.

        @return None after one canonical candidate retains xcrun priority.
        @throws AssertionError If an equivalent executable remains duplicated.
        @note The PATH llvm-nm spelling contains a parent segment to exercise
          canonical rather than string-only comparison.
        """

        harness = SymbolToolHarness(
            {
                "xcrun": "/usr/bin/xcrun",
                "llvm-nm": "/toolchain/../toolchain/shared-nm",
                "nm": "/toolchain/shared-nm",
            },
            {
                "/usr/bin/xcrun": (0, "/toolchain/shared-nm\n"),
            },
            {"/toolchain/shared-nm"},
        )

        resolution = static_product.resolve_product_archive_symbol_tools(
            "Darwin",
            pathlib.Path("/fixture/install-prefix"),
            which=harness.which,
            captured_runner=harness.run,
            executable_validator=harness.is_executable,
        )

        self.assertEqual(
            resolution.candidates,
            (
                static_product.SymbolToolCandidate(
                    source="xcrun llvm-nm",
                    executable="/toolchain/shared-nm",
                ),
            ),
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
    live_options, forwarded_unittest_argv = (
        parse_live_inventory_arguments(sys.argv)
    )
    InstallConsumerCTestRegistrationTest.live_build_dir = (
        live_options.build_dir
    )
    InstallConsumerCTestRegistrationTest.live_ctest_executable = (
        live_options.ctest_executable
    )
    InstallConsumerCTestRegistrationTest.live_config = live_options.config
    InstallConsumerCTestRegistrationTest.live_python_executable = (
        live_options.python_executable
    )
    InstallConsumerCTestRegistrationTest.live_expected_test_names = tuple(
        live_options.expected_smoke
    )
    unittest.main(argv=forwarded_unittest_argv)
