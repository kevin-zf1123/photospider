#!/usr/bin/env python3
"""Deterministic contracts for split photospiderd install-layout smokes."""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

import photospiderd_install_layout_smoke as subject


#: @brief Stable CTest name to strict CLI selector mapping.
#: @note Production CMake owns these registrations; the test reads the live
#:   configured inventory instead of parsing source text.
EXPECTED_LAYOUT_TESTS = {
    "PhotospiderdInstallLayoutNestedRelativeSmoke": "nested-relative",
    "PhotospiderdInstallLayoutAbsoluteLibdirSmoke": "absolute-libdir",
    "PhotospiderdInstallLayoutAbsoluteBindirSmoke": "absolute-bindir",
}
#: @brief Removed aggregate test name that must never reappear in inventory.
#: @note Keeping this value explicit protects the stable split boundary.
REMOVED_AGGREGATE_TEST = "PhotospiderdInstallLayoutSmoke"


def command_option_value(command: list[str], option: str) -> str:
    """@brief Read one exactly-once option value from a CTest command.

    @param command Shell-free argv serialized by CTest JSON.
    @param option Exact option expected once with one following value.
    @return The option's following value.
    @throws AssertionError If the option is absent, repeated, or valueless.
    @note This fail-closed reader prevents a duplicate flag from hiding a
      forged first or last value in registration regressions.
    """

    indices = [
        index for index, argument in enumerate(command) if argument == option
    ]
    if len(indices) != 1 or indices[0] + 1 >= len(command):
        raise AssertionError(
            f"expected exactly one valued {option!r} in {command!r}"
        )
    return command[indices[0] + 1]


class LayoutSelectionTest(unittest.TestCase):
    """@brief Verify strict single-case layout selection and cleanup.

    @throws AssertionError If selectors drift, argparse accepts an incomplete
      invocation, or one invocation can execute more than one layout.
    @note Tests mock only the expensive nested build; production path
      validation and cleanup execute against disposable real directories.
    """

    def test_layout_matrix_has_exact_unique_selectors(self) -> None:
        """@brief Pin the three maintained GNUInstallDirs case identities.

        @return None after names and absolute-destination ownership match.
        @throws AssertionError If the matrix is missing, duplicated, or moves
          an absolute destination outside its case-owned work root.
        @note Default relative ``bin``/``lib`` remains owned by the separate
          static-product consumer smoke and is intentionally absent here.
        """

        work = pathlib.Path("/synthetic/layout-work")
        layouts = subject.configured_layouts(work)
        self.assertEqual(
            tuple(layout.name for layout in layouts),
            subject.INSTALL_LAYOUT_NAMES,
        )
        self.assertEqual(len(set(subject.INSTALL_LAYOUT_NAMES)), 3)
        self.assertEqual(layouts[0].bindir, "libexec/photospider")
        self.assertEqual(layouts[0].libdir, "lib64")
        self.assertEqual(
            pathlib.Path(layouts[1].libdir),
            work / "absolute-libdir" / "runtime",
        )
        self.assertEqual(
            pathlib.Path(layouts[2].bindir),
            work / "absolute-bindir" / "daemon",
        )

    def test_cli_rejects_missing_and_unknown_layout(self) -> None:
        """@brief Require an explicit selector before filesystem inspection.

        @return None after argparse rejects missing and unknown selectors.
        @throws AssertionError If either incomplete invocation reaches runtime.
        @note Synthetic paths need not exist because strict CLI validation must
          fail before platform or filesystem behavior begins.
        """

        base_arguments = [
            "--repo",
            "/synthetic/repo",
            "--build-root",
            "/synthetic/build",
            "--work",
            "/synthetic/build/work",
        ]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                subject.main(base_arguments)
            with self.assertRaises(SystemExit):
                subject.main([*base_arguments, "--layout", "unknown"])

    def test_each_invocation_executes_only_its_selected_layout(self) -> None:
        """@brief Execute every selector through the real CLI boundary.

        @return None after each invocation calls ``run_layout`` exactly once
          with the requested record and removes only its isolated work tree.
        @throws OSError If disposable directories cannot be created.
        @throws AssertionError If selection, isolation, or cleanup drifts.
        @note The expensive configure/build/install helper is mocked; argument
          parsing, path validation, matrix resolution, and final cleanup are
          production code.
        """

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = pathlib.Path(temporary_directory)
            repo = root / "repo"
            build_root = root / "build"
            repo.mkdir()
            build_root.mkdir()
            for selector in subject.INSTALL_LAYOUT_NAMES:
                with self.subTest(selector=selector):
                    work = build_root / selector
                    expected_work = work.resolve(strict=False)
                    with (
                        mock.patch.object(
                            subject.platform, "system", return_value="Linux"
                        ),
                        mock.patch.object(subject, "run_layout") as run_layout,
                    ):
                        result = subject.main(
                            [
                                "--repo",
                                str(repo),
                                "--build-root",
                                str(build_root),
                                "--work",
                                str(work),
                                "--layout",
                                selector,
                            ]
                        )
                    self.assertEqual(result, 0)
                    run_layout.assert_called_once()
                    selected = run_layout.call_args.args[0]
                    self.assertEqual(selected.name, selector)
                    self.assertEqual(
                        run_layout.call_args.kwargs["work"], expected_work
                    )
                    self.assertFalse(work.exists())


class CTestRegistrationTest(unittest.TestCase):
    """@brief Verify the live configured CTest layout inventory.

    @throws OSError If CTest cannot start.
    @throws subprocess.CalledProcessError If inventory generation fails.
    @throws AssertionError If names, commands, isolation, or properties drift.
    @note Direct unittest runs skip this class unless CMake supplies its build
      directory and CTest executable through the registered test environment.
    """

    def test_live_inventory_contains_only_three_split_layout_smokes(self) -> None:
        """@brief Inspect CTest JSON rather than parsing CMake source text.

        @return None after the aggregate is absent and every split test binds
          one exact selector, one unique work root, and build-smoke isolation.
        @throws unittest.SkipTest If no configured inventory was supplied.
        @throws OSError If CTest cannot start.
        @throws subprocess.CalledProcessError If CTest rejects the query.
        @throws AssertionError If any registration contract differs.
        @note ``RUN_SERIAL`` remains true within a CTest process; CI may route
          these independently named cases to separate jobs.
        """

        ctest_executable = os.environ.get("PHOTOSPIDER_CTEST_EXECUTABLE", "")
        build_directory = os.environ.get("PHOTOSPIDER_BUILD_DIRECTORY", "")
        if not ctest_executable or not build_directory:
            self.skipTest("live CTest inventory was not supplied")
        completed = subprocess.run(
            [
                ctest_executable,
                "--test-dir",
                build_directory,
                "--show-only=json-v1",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        payload = json.loads(completed.stdout)
        tests = {entry["name"]: entry for entry in payload["tests"]}
        self.assertNotIn(REMOVED_AGGREGATE_TEST, tests)
        layout_smoke_names = {
            name
            for name in tests
            if name.startswith("PhotospiderdInstallLayout")
            and name.endswith("Smoke")
        }
        self.assertEqual(layout_smoke_names, set(EXPECTED_LAYOUT_TESTS))

        work_roots: set[str] = set()
        configured_build = pathlib.Path(build_directory).resolve(strict=True)
        for test_name, selector in EXPECTED_LAYOUT_TESTS.items():
            entry = tests[test_name]
            command = entry["command"]
            self.assertEqual(
                command_option_value(command, "--layout"), selector
            )
            work_root = command_option_value(command, "--work")
            self.assertNotIn(work_root, work_roots)
            work_roots.add(work_root)
            resolved_work = pathlib.Path(work_root).resolve(strict=False)
            self.assertIn(configured_build, resolved_work.parents)
            properties = {
                property_entry["name"]: property_entry["value"]
                for property_entry in entry.get("properties", [])
            }
            self.assertTrue(properties["RUN_SERIAL"])
            self.assertEqual(float(properties["TIMEOUT"]), 900.0)
            self.assertIn("build-smoke", properties["LABELS"])
        self.assertEqual(len(work_roots), len(EXPECTED_LAYOUT_TESTS))


if __name__ == "__main__":
    unittest.main()
