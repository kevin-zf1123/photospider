#!/usr/bin/env python3
"""@file build_smoke_inventory_test.py
@brief Regress deterministic and fail-closed build-smoke inventory handling.

The suite combines in-memory malformed-inventory cases, mocked runner process
boundaries, and one disposable real CMake/CTest POST_BUILD discovery fixture.

@note Every filesystem fixture is scoped to a TemporaryDirectory. Tests retain
  no cache, build tree, child process, or shared cross-thread state.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


#: @brief Repository script loaded as the exact system under test.
#: @note The path is resolved once for this test-process lifetime.
MODULE_PATH = Path(__file__).with_name("build_smoke_inventory.py")
#: @brief Import specification for loading the standalone script as a module.
#: @note The specification owns no file handle after module execution.
SPEC = importlib.util.spec_from_file_location("build_smoke_inventory", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load {MODULE_PATH}")
#: @brief Process-local loaded module used by every focused regression.
#: @note Registration in sys.modules supports dataclass initialization; the
#:   module is removed automatically when the test process exits.
inventory_module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = inventory_module
SPEC.loader.exec_module(inventory_module)


def test_entry(
    name: str,
    *,
    labels: list[str] | None = None,
    disabled: bool = False,
    command: list[str] | None = None,
) -> dict[str, object]:
    """@brief Create one realistic CTest JSON test entry.

    @param name Exact CTest test name.
    @param labels Optional LABELS array; None omits the property.
    @param disabled Whether to add the boolean DISABLED property.
    @param command Optional command argv; None omits the command field.
    @return Newly owned JSON-compatible test-entry dictionary.
    @throws No declared exception for validated test inputs.
    @note The helper performs no I/O and shares no mutable containers between
      calls.
    """

    properties: list[dict[str, object]] = [
        {"name": "WORKING_DIRECTORY", "value": "/tmp/build"}
    ]
    if labels is not None:
        properties.append({"name": "LABELS", "value": labels})
    if disabled:
        properties.append({"name": "DISABLED", "value": True})
    entry: dict[str, object] = {
        "name": name,
        "properties": properties,
    }
    if command is not None:
        entry["command"] = command
    return entry


def inventory_payload(tests: list[dict[str, object]]) -> bytes:
    """@brief Serialize one minimal CTest json-v1 inventory.

    @param tests JSON-compatible test entries in desired CTest index order.
    @return UTF-8 encoded ctestInfo version 1 payload.
    @throws TypeError If a supplied entry is not JSON serializable.
    @throws UnicodeError If a supplied string cannot be encoded as UTF-8.
    @note Serialization is in-memory and does not retain the mutable test list.
    """

    return json.dumps(
        {
            "kind": "ctestInfo",
            "tests": tests,
            "version": {"major": 1, "minor": 0},
        }
    ).encode("utf-8")


class InventoryParsingTest(unittest.TestCase):
    """@brief Exercise strict JSON schema, label, and matrix validation.

    @throws AssertionError If malformed state is accepted or deterministic
      output changes.
    @note Cases are synchronous and use only in-memory JSON values.
    """

    def test_matrix_is_sorted_safe_and_round_trippable(self) -> None:
        """@brief Preserve exact hostile names in deterministic safe output.

        @return None after ordering, JSON round trip, and artifact-key checks.
        @throws AssertionError If names are reordered unsafely, raw newlines
          enter JSON, or artifact keys collide or contain unsafe characters.
        @note No subprocess or filesystem I/O occurs.
        """

        raw = inventory_payload(
            [
                test_entry(
                    "zeta [consumer]",
                    labels=["build-smoke", "package"],
                    command=["python3", "zeta.py"],
                ),
                test_entry("ordinary", command=["ordinary"]),
                test_entry(
                    'alpha "quoted"\nline',
                    labels=["build-smoke"],
                    command=["python3", "alpha.py"],
                ),
                test_entry(
                    "Unicode-测试",
                    labels=["build-smoke"],
                    command=["python3", "unicode.py"],
                ),
            ]
        )
        inventory = inventory_module.parse_inventory(raw)
        matrix_text, selected = inventory_module.build_matrix(inventory.build_smokes())
        matrix = json.loads(matrix_text)

        self.assertEqual(
            [test.name for test in selected],
            ['alpha "quoted"\nline', "Unicode-测试", "zeta [consumer]"],
        )
        self.assertNotIn("\n", matrix_text)
        self.assertEqual(
            [entry["test"] for entry in matrix["include"]],
            [test.name for test in selected],
        )
        artifacts = [entry["artifact"] for entry in matrix["include"]]
        self.assertEqual(len(artifacts), len(set(artifacts)))
        for artifact in artifacts:
            self.assertRegex(artifact, r"^[a-z0-9-]+$")

    def test_duplicate_test_name_fails_closed(self) -> None:
        """@brief Reject duplicate names in the complete CTest inventory.

        @return None after the duplicate-name diagnostic is observed.
        @throws AssertionError If duplicate identity is accepted.
        @note The parser receives an in-memory payload and creates no artifacts.
        """

        raw = inventory_payload(
            [
                test_entry("duplicate", command=["one"]),
                test_entry("duplicate", command=["two"]),
            ]
        )
        with self.assertRaisesRegex(inventory_module.InventoryError, "duplicated"):
            inventory_module.parse_inventory(raw)

    def test_lone_surrogate_name_reports_controlled_main_error(self) -> None:
        """@brief Report an invalid Unicode test name without a traceback.

        @return None after the stable diagnostic and status are verified.
        @throws AssertionError If a lone surrogate escapes InventoryError
          handling, changes the diagnostic, or produces a traceback.
        @note JSON serialization escapes the surrogate before UTF-8 encoding.
          Discovery is mocked and all output paths are temporary.
        """

        payload = inventory_payload(
            [
                test_entry(
                    "\ud800",
                    labels=["build-smoke"],
                    command=["invalid-name"],
                )
            ]
        )
        query_result = subprocess.CompletedProcess(
            args=["ctest"], returncode=0, stdout=payload, stderr=b""
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            stderr = io.StringIO()
            with mock.patch.object(
                inventory_module.subprocess,
                "run",
                return_value=query_result,
            ):
                with contextlib.redirect_stderr(stderr):
                    status = inventory_module.main(
                        [
                            "plan",
                            "--build-dir",
                            str(temporary_root / "build"),
                            "--inventory-output",
                            str(temporary_root / "inventory.json"),
                            "--matrix-output",
                            str(temporary_root / "matrix.json"),
                        ]
                    )

        diagnostic = stderr.getvalue()
        self.assertEqual(status, 2)
        self.assertEqual(
            diagnostic,
            "Build-smoke inventory error: "
            "CTest test 1 name is not valid UTF-8.\n",
        )
        self.assertNotIn("Traceback", diagnostic)

    def test_duplicate_labels_property_fails_closed(self) -> None:
        """@brief Reject two LABELS property objects on one test.

        @return None after duplicate property identity is rejected.
        @throws AssertionError If the later property can overwrite the first.
        @note This differs from duplicate values inside one LABELS array.
        """

        entry = test_entry("smoke", labels=["build-smoke"], command=["smoke"])
        entry["properties"].append({"name": "LABELS", "value": ["build-smoke"]})
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "repeats property"
        ):
            inventory_module.parse_inventory(inventory_payload([entry]))

    def test_duplicate_label_values_fail_closed(self) -> None:
        """@brief Reject a repeated value inside one LABELS array.

        @return None after the exact repeated label is diagnosed.
        @throws AssertionError If a duplicate label value is normalized away.
        @note The case directly covers the set-based duplicate-value guard and
          performs no subprocess or filesystem I/O.
        """

        entry = test_entry(
            "smoke",
            labels=["build-smoke", "build-smoke"],
            command=["smoke"],
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "repeats 'build-smoke'"
        ):
            inventory_module.parse_inventory(inventory_payload([entry]))

    def test_malformed_label_value_fails_closed(self) -> None:
        """@brief Reject a scalar LABELS value instead of an array.

        @return None after the malformed shape is diagnosed.
        @throws AssertionError If schema validation accepts the scalar.
        @note The case isolates LABELS shape from command validation.
        """

        entry = test_entry("smoke", command=["smoke"])
        entry["properties"].append({"name": "LABELS", "value": "build-smoke"})
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "must be an array"
        ):
            inventory_module.parse_inventory(inventory_payload([entry]))

    def test_empty_build_smoke_set_fails_closed(self) -> None:
        """@brief Keep authoritative label selection strict on an empty set.

        @return None after default selection rejects the empty labelled set.
        @throws AssertionError If strict post-build planning can emit no jobs.
        @note Configuration preflight uses an explicit allow-empty flag and is
          covered separately by the real POST_BUILD fixture.
        """

        inventory = inventory_module.parse_inventory(
            inventory_payload([test_entry("ordinary", command=["ordinary"])])
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "no tests with exact label"
        ):
            inventory.build_smokes()

    def test_disabled_or_commandless_build_smoke_fails_closed(self) -> None:
        """@brief Reject labelled tests that CTest cannot execute.

        @return None after disabled and commandless entries both fail.
        @throws AssertionError If either unusable entry reaches matrix output.
        @note Both inventories are parsed in memory without launching CTest.
        """

        disabled = inventory_module.parse_inventory(
            inventory_payload(
                [
                    test_entry(
                        "disabled",
                        labels=["build-smoke"],
                        disabled=True,
                        command=["disabled"],
                    )
                ]
            )
        )
        with self.assertRaisesRegex(inventory_module.InventoryError, "disabled"):
            disabled.build_smokes()

        commandless = inventory_module.parse_inventory(
            inventory_payload([test_entry("commandless", labels=["build-smoke"])])
        )
        with self.assertRaisesRegex(
            inventory_module.InventoryError, "no executable CTest command"
        ):
            commandless.build_smokes()

    def test_unsupported_schema_and_invalid_json_fail_closed(self) -> None:
        """@brief Reject malformed JSON, kind, and boolean schema versions.

        @return None after each independent schema violation is diagnosed.
        @throws AssertionError If any malformed root is accepted.
        @note Inputs are intentionally minimal and perform no external I/O.
        """

        with self.assertRaisesRegex(inventory_module.InventoryError, "not valid JSON"):
            inventory_module.parse_inventory(b"{")
        wrong_kind = json.dumps(
            {
                "kind": "cmakeFiles",
                "tests": [test_entry("smoke", command=["smoke"])],
                "version": {"major": 1, "minor": 0},
            }
        )
        with self.assertRaisesRegex(inventory_module.InventoryError, "kind"):
            inventory_module.parse_inventory(wrong_kind)
        boolean_major = json.dumps(
            {
                "kind": "ctestInfo",
                "tests": [test_entry("smoke", command=["smoke"])],
                "version": {"major": True, "minor": 0},
            }
        )
        with self.assertRaisesRegex(inventory_module.InventoryError, "json-v1"):
            inventory_module.parse_inventory(boolean_major)


class ExactExecutionTest(unittest.TestCase):
    """@brief Prove runner revalidation precedes index-only execution.

    @throws AssertionError If a selected name bypasses exact inventory checks or
      enters the execution argv.
    @note subprocess.run is mocked, so no real CTest process is started.
    """

    def _assert_selected_rejected(
        self,
        tests: list[dict[str, object]],
        selected_name: str,
        expected_diagnostic: str,
    ) -> None:
        """@brief Require one fresh-inventory selection to stop before execution.

        The helper builds runner arguments, supplies one mocked discovery result,
        checks the expected InventoryError, and proves no second subprocess call
        or selection artifact occurs.

        @param tests Complete mocked CTest test-entry inventory.
        @param selected_name Exact name requested by the runner.
        @param expected_diagnostic Regular expression for the rejection message.
        @return None after the fail-closed subprocess boundary is verified.
        @throws AssertionError If validation succeeds, reports the wrong reason,
          calls subprocess.run twice, or writes a selection artifact.
        @note The temporary artifact tree is removed before return. Mock state is
          local to this call and no background work exists.
        """

        payload = inventory_payload(tests)
        query_result = subprocess.CompletedProcess(
            args=["ctest"], returncode=0, stdout=payload, stderr=b""
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            selection_path = temporary_root / "selection.json"
            args = inventory_module.parse_args(
                [
                    "run",
                    "--build-dir",
                    str(temporary_root / "build"),
                    "--inventory-output",
                    str(temporary_root / "inventory.json"),
                    "--selection-output",
                    str(selection_path),
                    "--test-name",
                    selected_name,
                ]
            )
            with mock.patch.object(
                inventory_module.subprocess,
                "run",
                return_value=query_result,
            ) as run_mock:
                with self.assertRaisesRegex(
                    inventory_module.InventoryError, expected_diagnostic
                ):
                    inventory_module.run_selected(args)
            run_mock.assert_called_once()
            self.assertFalse(selection_path.exists())

    def test_selected_test_uses_only_validated_numeric_index(self) -> None:
        """@brief Execute an adversarial exact name only through its CTest index.

        @return None after discovery, execution argv, and selection artifact
          checks pass.
        @throws AssertionError If the name reaches a shell/regex argument, the
          wrong index executes, or exact identity is lost in diagnostics.
        @note Both subprocess calls are mocked and all artifacts are temporary.
        """

        adversarial_name = "smoke; $(touch should-not-run) [.*]\nnext"
        payload = inventory_payload(
            [
                test_entry("ordinary", command=["ordinary"]),
                test_entry(
                    adversarial_name,
                    labels=["build-smoke"],
                    command=["python3", "runner.py"],
                ),
            ]
        )
        query_result = subprocess.CompletedProcess(
            args=["ctest"], returncode=0, stdout=payload, stderr=b""
        )
        run_result = subprocess.CompletedProcess(args=["ctest"], returncode=0)
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            args = inventory_module.parse_args(
                [
                    "run",
                    "--build-dir",
                    str(temporary_root / "build"),
                    "--inventory-output",
                    str(temporary_root / "inventory.json"),
                    "--selection-output",
                    str(temporary_root / "selection.json"),
                    "--test-name",
                    adversarial_name,
                ]
            )
            with mock.patch.object(
                inventory_module.subprocess,
                "run",
                side_effect=[query_result, run_result],
            ) as run_mock:
                self.assertEqual(inventory_module.run_selected(args), 0)

            execution_argv = run_mock.call_args_list[1].args[0]
            self.assertIn("-I", execution_argv)
            self.assertIn("2,2", execution_argv)
            self.assertNotIn("-L", execution_argv)
            self.assertNotIn(adversarial_name, execution_argv)
            selection = json.loads(
                (temporary_root / "selection.json").read_text(encoding="utf-8")
            )
            self.assertEqual(selection["test"], adversarial_name)

    def test_selected_unlabelled_test_fails_before_execution(self) -> None:
        """@brief Reject an exact name that lacks the build-smoke label.

        @return None after only the discovery subprocess call is observed.
        @throws AssertionError If the runner executes the unlabelled test.
        @note The shared rejection helper also verifies no selection artifact.
        """

        self._assert_selected_rejected(
            [test_entry("ordinary", command=["ordinary"])],
            "ordinary",
            "lacks exact label",
        )

    def test_selected_absent_test_fails_before_execution(self) -> None:
        """@brief Reject a selected name absent from fresh CTest inventory.

        @return None after only the discovery subprocess call is observed.
        @throws AssertionError If an absent name reaches CTest execution.
        @note The selected name never appears in the mocked inventory.
        """

        self._assert_selected_rejected(
            [test_entry("ordinary", command=["ordinary"])],
            "missing-smoke",
            "absent from CTest inventory",
        )

    def test_selected_disabled_test_fails_before_execution(self) -> None:
        """@brief Reject a freshly disabled exact labelled selection.

        @return None after only the discovery subprocess call is observed.
        @throws AssertionError If disabled state is ignored or execution starts.
        @note The exact label and command are present so the disabled branch is
          the sole rejection reason.
        """

        self._assert_selected_rejected(
            [
                test_entry(
                    "disabled-smoke",
                    labels=["build-smoke"],
                    disabled=True,
                    command=["disabled-smoke"],
                )
            ],
            "disabled-smoke",
            "is disabled",
        )

    def test_selected_commandless_test_fails_before_execution(self) -> None:
        """@brief Reject a freshly commandless exact labelled selection.

        @return None after only the discovery subprocess call is observed.
        @throws AssertionError If missing command state reaches execution.
        @note The exact label is present and DISABLED is false, isolating the
          command validation branch.
        """

        self._assert_selected_rejected(
            [
                test_entry(
                    "commandless-smoke",
                    labels=["build-smoke"],
                )
            ],
            "commandless-smoke",
            "has no CTest command",
        )


class PostBuildDiscoveryTest(unittest.TestCase):
    """@brief Exercise real CMake POST_BUILD GoogleTest discovery timing.

    @throws AssertionError If configuration placeholders become authoritative or
      the post-build labelled test is omitted from the strict matrix.
    @note The fixture compiles one tiny C executable in a disposable directory.
      It uses no repository build tree and removes all generated content.
    """

    def test_configure_placeholder_cannot_omit_post_build_label(self) -> None:
        """@brief Prefer the strict post-build matrix over configure inventory.

        The test configures a minimal target using the production
        ``gtest_discover_tests`` default, verifies configure-time CTest exposes
        only an unlabelled ``_NOT_BUILT`` placeholder, builds the target, and
        verifies strict planning then emits the discovered labelled test.

        @return None after preflight and authoritative matrices match their
          respective lifecycle phases.
        @throws AssertionError If required tools are absent, fixture commands
          fail, preflight is not safely empty, or post-build identity is omitted.
        @note Commands run synchronously without a shell. The tiny fixture
          executable implements only GoogleTest list/run argument behavior and
          all source/build/artifact paths die with the TemporaryDirectory.
        """

        cmake_executable = shutil.which("cmake")
        ctest_executable = shutil.which("ctest")
        self.assertIsNotNone(cmake_executable)
        self.assertIsNotNone(ctest_executable)

        with tempfile.TemporaryDirectory(
            prefix="photospider-build-smoke-post-build-"
        ) as temporary_directory:
            temporary_root = Path(temporary_directory)
            source_dir = temporary_root / "source"
            build_dir = temporary_root / "build"
            preflight_dir = temporary_root / "preflight"
            authoritative_dir = temporary_root / "authoritative"
            source_dir.mkdir()
            (source_dir / "CMakeLists.txt").write_text(
                "\n".join(
                    [
                        "cmake_minimum_required(VERSION 3.16)",
                        "project(PostBuildSmokeFixture LANGUAGES C)",
                        "include(CTest)",
                        "include(GoogleTest)",
                        "add_executable(post_build_fixture fixture.c)",
                        "gtest_discover_tests(post_build_fixture",
                        '  PROPERTIES LABELS "build-smoke")',
                        "",
                    ]
                ),
                encoding="utf-8",
            )
            (source_dir / "fixture.c").write_text(
                "\n".join(
                    [
                        "#include <stdio.h>",
                        "#include <string.h>",
                        "",
                        "int main(int argc, char **argv) {",
                        "  int index = 0;",
                        "  for (index = 1; index < argc; ++index) {",
                        '    if (strcmp(argv[index], "--gtest_list_tests") == 0) {',
                        '      puts("PostBuildFixture.");',
                        '      puts("  LabelledSmoke");',
                        "      return 0;",
                        "    }",
                        "  }",
                        "  return 0;",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            subprocess.run(
                [
                    str(cmake_executable),
                    "-S",
                    str(source_dir),
                    "-B",
                    str(build_dir),
                    "-DCMAKE_BUILD_TYPE=Release",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            preflight_status = inventory_module.main(
                [
                    "plan",
                    "--build-dir",
                    str(build_dir),
                    "--ctest-executable",
                    str(ctest_executable),
                    "--config",
                    "Release",
                    "--inventory-output",
                    str(preflight_dir / "inventory.json"),
                    "--matrix-output",
                    str(preflight_dir / "matrix.json"),
                    "--names-output",
                    str(preflight_dir / "names.z"),
                    "--allow-empty",
                ]
            )
            self.assertEqual(preflight_status, 0)
            preflight_inventory = json.loads(
                (preflight_dir / "inventory.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                [test["name"] for test in preflight_inventory["tests"]],
                ["post_build_fixture_NOT_BUILT"],
            )
            self.assertEqual(
                json.loads((preflight_dir / "matrix.json").read_text(encoding="utf-8")),
                {"include": []},
            )
            self.assertEqual((preflight_dir / "names.z").read_bytes(), b"")

            subprocess.run(
                [
                    str(cmake_executable),
                    "--build",
                    str(build_dir),
                    "--target",
                    "post_build_fixture",
                    "--config",
                    "Release",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            authoritative_status = inventory_module.main(
                [
                    "plan",
                    "--build-dir",
                    str(build_dir),
                    "--ctest-executable",
                    str(ctest_executable),
                    "--config",
                    "Release",
                    "--inventory-output",
                    str(authoritative_dir / "inventory.json"),
                    "--matrix-output",
                    str(authoritative_dir / "matrix.json"),
                    "--names-output",
                    str(authoritative_dir / "names.z"),
                ]
            )
            self.assertEqual(authoritative_status, 0)
            authoritative_matrix = json.loads(
                (authoritative_dir / "matrix.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                [entry["test"] for entry in authoritative_matrix["include"]],
                ["PostBuildFixture.LabelledSmoke"],
            )
            self.assertEqual(
                (authoritative_dir / "names.z").read_bytes(),
                b"PostBuildFixture.LabelledSmoke\0",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
