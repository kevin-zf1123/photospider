#!/usr/bin/env python3
"""Deterministic safety regressions for the optional-provider build smoke."""

from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from unittest import mock

import optional_opencv_operation_provider_build_smoke as subject


class WorkDirectorySafetyTest(unittest.TestCase):
    """@brief Verifies destructive work-path handling in an isolated tree.

    @throws AssertionError When the smoke accepts or mutates a protected path.
    @note Every repository and ancestor used here is synthetic and lives under
      a test-owned temporary directory; no real checkout path is passed to the
      destructive helper.
    """

    def test_rejects_synthetic_repository_and_ancestor(self) -> None:
        """@brief Require repository and ancestor rejection before deletion.

        @return None after both synthetic paths remain intact.
        @throws AssertionError If either path is accepted or its marker changes.
        @note Candidates are disposable test directories, never the real
          Photospider checkout or one of its parents.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-provider-work-safety-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_parent = sandbox / "checkout"
            synthetic_repo = synthetic_parent / "photospider"
            synthetic_repo.mkdir(parents=True)
            marker = synthetic_repo / "must-survive"
            marker.write_text("synthetic", encoding="utf-8")

            for candidate in (synthetic_repo, synthetic_parent):
                with self.subTest(candidate=candidate):
                    with self.assertRaises(ValueError):
                        subject.remove_work_tree(candidate, synthetic_repo)
                    self.assertEqual(
                        marker.read_text(encoding="utf-8"), "synthetic"
                    )

    def test_rejects_symlinks_to_synthetic_protected_paths(self) -> None:
        """@brief Require resolved symlinks to protected paths to be rejected.

        @return None after both synthetic targets remain intact.
        @throws AssertionError If a symlink target is accepted or mutated.
        @note The case is skipped only when the host cannot create directory
          symlinks. Any created link and target remain inside the temporary
          sandbox.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-provider-work-symlink-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_parent = sandbox / "checkout"
            synthetic_repo = synthetic_parent / "photospider"
            synthetic_repo.mkdir(parents=True)
            marker = synthetic_repo / "must-survive"
            marker.write_text("synthetic", encoding="utf-8")

            candidates: list[pathlib.Path] = []
            try:
                repo_link = sandbox / "repo-link"
                repo_link.symlink_to(synthetic_repo, target_is_directory=True)
                candidates.append(repo_link)
                parent_link = sandbox / "parent-link"
                parent_link.symlink_to(
                    synthetic_parent, target_is_directory=True
                )
                candidates.append(parent_link)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"directory symlinks unavailable: {error}")

            for candidate in candidates:
                with self.subTest(candidate=candidate):
                    with self.assertRaises(ValueError):
                        subject.remove_work_tree(candidate, synthetic_repo)
                    self.assertEqual(
                        marker.read_text(encoding="utf-8"), "synthetic"
                    )

    def test_propagates_removal_failure_and_checks_postcondition(self) -> None:
        """@brief Require deletion errors and silent no-op deletion to fail.

        @return None after both injected failure modes are observed.
        @throws AssertionError If an injected failure is hidden.
        @note The recursive remover is mocked, so the test never attempts to
          delete anything outside its synthetic work directory.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-provider-work-failure-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            work = sandbox / "work"
            work.mkdir()

            with mock.patch.object(
                subject.shutil,
                "rmtree",
                side_effect=OSError("injected recursive removal failure"),
            ):
                with self.assertRaisesRegex(
                    OSError, "injected recursive removal failure"
                ):
                    subject.remove_work_tree(work, synthetic_repo)
            self.assertTrue(work.is_dir())

            with mock.patch.object(subject.shutil, "rmtree", return_value=None):
                with self.assertRaisesRegex(
                    RuntimeError, "directory still exists"
                ):
                    subject.remove_work_tree(work, synthetic_repo)
            self.assertTrue(work.is_dir())

    def test_removes_only_valid_synthetic_work_tree(self) -> None:
        """@brief Require successful removal of a safe synthetic work tree.

        @return None after the transient child no longer exists.
        @throws AssertionError If the safe work tree survives or the repository
          marker is changed.
        @note The only real recursive deletion targets a disposable child of
          the test-owned temporary sandbox.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-provider-work-success-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            marker = synthetic_repo / "must-survive"
            marker.write_text("synthetic", encoding="utf-8")
            work = sandbox / "work"
            work.mkdir()
            (work / "stale").write_text("stale", encoding="utf-8")

            resolved = subject.remove_work_tree(work, synthetic_repo)

            self.assertEqual(resolved, work.resolve())
            self.assertFalse(work.exists())
            self.assertEqual(marker.read_text(encoding="utf-8"), "synthetic")


class ConfigurationLayoutTest(unittest.TestCase):
    """@brief Verifies cache-driven executable layout without running CMake.

    @throws AssertionError When cache interpretation contradicts generator mode.
    @note Every cache is synthetic and isolated under a temporary directory.
    """

    def test_resolves_single_and_multi_config_layouts_from_cache(self) -> None:
        """@brief Require cache metadata, not platform, to choose subdirectories.

        @return None after single- and multi-config paths match their caches.
        @throws AssertionError If either cached generator mode resolves
          incorrectly.
        @note Platform identity is used only for the executable suffix.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-provider-cache-layout-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            executable_name = (
                "test_optional_opencv_operation_provider"
                + (".exe" if os.name == "nt" else "")
            )

            single_build = sandbox / "single"
            single_build.mkdir()
            (single_build / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
                "CMAKE_GENERATOR:INTERNAL=Ninja\n",
                encoding="utf-8",
            )
            self.assertEqual(
                subject.configured_test_executable(
                    single_build, "RelWithDebInfo"
                ),
                single_build / "tests" / executable_name,
            )

            multi_build = sandbox / "multi"
            multi_build.mkdir()
            (multi_build / "CMakeCache.txt").write_text(
                "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release;"
                "RelWithDebInfo\n"
                "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n",
                encoding="utf-8",
            )
            self.assertEqual(
                subject.configured_test_executable(
                    multi_build, "RelWithDebInfo"
                ),
                multi_build / "tests" / "RelWithDebInfo" / executable_name,
            )

    def test_rejects_missing_or_mismatched_configuration_metadata(self) -> None:
        """@brief Require incomplete or contradictory cache state to fail.

        @return None after every malformed synthetic cache is rejected.
        @throws AssertionError If missing or mismatched metadata is accepted.
        @note The cases cover absent build type, single-config mismatch,
          multi-config mismatch, and a missing cache file.
        """

        with tempfile.TemporaryDirectory(
            prefix="photospider-provider-cache-errors-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)

            missing_cache = sandbox / "missing-cache"
            missing_cache.mkdir()
            with self.assertRaisesRegex(RuntimeError, "has no cache"):
                subject.configured_test_executable(
                    missing_cache, "RelWithDebInfo"
                )

            missing_build_type = sandbox / "missing-build-type"
            missing_build_type.mkdir()
            (missing_build_type / "CMakeCache.txt").write_text(
                "CMAKE_GENERATOR:INTERNAL=Ninja\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(RuntimeError, "CMAKE_BUILD_TYPE"):
                subject.configured_test_executable(
                    missing_build_type, "RelWithDebInfo"
                )

            single_mismatch = sandbox / "single-mismatch"
            single_mismatch.mkdir()
            (single_mismatch / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Debug\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(RuntimeError, "build type mismatch"):
                subject.configured_test_executable(
                    single_mismatch, "RelWithDebInfo"
                )

            multi_mismatch = sandbox / "multi-mismatch"
            multi_mismatch.mkdir()
            (multi_mismatch / "CMakeCache.txt").write_text(
                "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "requires --config"):
                subject.configured_test_executable(multi_mismatch, "")
            with self.assertRaisesRegex(
                RuntimeError, "configuration mismatch"
            ):
                subject.configured_test_executable(
                    multi_mismatch, "RelWithDebInfo"
                )


if __name__ == "__main__":
    unittest.main()
