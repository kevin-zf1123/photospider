#!/usr/bin/env python3
"""Deterministic safety regressions for the optional-provider build smoke."""

from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from unittest import mock

import optional_opencv_operation_provider_build_smoke as subject


def synthetic_temporary_directory(
    prefix: str,
) -> tempfile.TemporaryDirectory:
    """@brief Create one disposable sandbox below a symlink-free temp root.

    @param prefix Unique test-purpose prefix for the sandbox basename.
    @return Temporary-directory context manager yielding a synthetic root.
    @throws OSError If the system temporary root cannot be resolved or the
      sandbox cannot be created.
    @throws RuntimeError If resolving the system temporary root encounters a
      symlink loop.
    @note macOS commonly exposes its real temporary root through `/var`, which
      is itself a symlink. Canonicalizing only the test-owned parent prevents
      that host alias from masking the explicit final- and parent-symlink
      cases. No real checkout path or parent is returned.
    """

    temporary_root = pathlib.Path(tempfile.gettempdir()).resolve()
    return tempfile.TemporaryDirectory(prefix=prefix, dir=temporary_root)


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

        with synthetic_temporary_directory(
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

    def test_rejects_absolute_parent_traversal_before_removal(self) -> None:
        """@brief Reject an absolute work spelling containing parent traversal.

        The test creates a disposable work tree, spells that same tree through
        an absolute `..` component, and invokes only the path-resolution guard.

        @return None after the guard rejects the spelling while the marker and
          mocked recursive remover remain untouched.
        @throws OSError If the synthetic tree cannot be created or inspected.
        @throws AssertionError If the spelling is not absolute, omits `..`, is
          accepted, reaches recursive removal, or changes the marker.
        @note Every path is synthetic and test-owned. The mocked remover is a
          safety sentinel; this test never requests recursive deletion.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-parent-traversal-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            work_parent = sandbox / "work-parent"
            work_parent.mkdir()
            work = sandbox / "work"
            work.mkdir()
            marker = work / "must-survive"
            marker.write_text("synthetic", encoding="utf-8")
            traversal_work = work_parent / os.pardir / work.name

            self.assertTrue(traversal_work.is_absolute())
            self.assertIn(os.pardir, traversal_work.parts)
            with mock.patch.object(subject.shutil, "rmtree") as remover:
                with self.assertRaisesRegex(ValueError, "parent traversal"):
                    subject.resolve_work_directory(
                        traversal_work, synthetic_repo
                    )
                remover.assert_not_called()
            self.assertEqual(
                marker.read_text(encoding="utf-8"), "synthetic"
            )

    def test_rejects_current_filesystem_root_during_resolution(self) -> None:
        """@brief Reject the current filesystem root through its dedicated guard.

        The test derives the active root from a disposable absolute path's
        anchor, then calls only `resolve_work_directory` with that root.

        @return None after the dedicated root diagnostic is observed and the
          synthetic repository marker remains unchanged.
        @throws OSError If the synthetic repository cannot be created or read.
        @throws AssertionError If anchor derivation is invalid, the root is
          accepted, recursive removal is reached, or the marker changes.
        @note The root is inspected only by the non-destructive parsing guard.
          No recursive-removal helper receives or operates on the root.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-filesystem-root-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            marker = synthetic_repo / "must-survive"
            marker.write_text("synthetic", encoding="utf-8")
            synthetic_absolute_path = sandbox / "anchor-probe"
            filesystem_root = pathlib.Path(synthetic_absolute_path.anchor)

            self.assertTrue(synthetic_absolute_path.is_absolute())
            self.assertTrue(filesystem_root.is_absolute())
            self.assertEqual(filesystem_root.parent, filesystem_root)
            with mock.patch.object(subject.shutil, "rmtree") as remover:
                with self.assertRaisesRegex(ValueError, "filesystem root"):
                    subject.resolve_work_directory(
                        filesystem_root, synthetic_repo
                    )
                remover.assert_not_called()
            self.assertEqual(
                marker.read_text(encoding="utf-8"), "synthetic"
            )

    def test_rejects_symlinks_to_synthetic_protected_paths(self) -> None:
        """@brief Require symlinks naming protected paths to be rejected.

        @return None after both synthetic targets remain intact.
        @throws AssertionError If a symlink target is accepted or mutated.
        @note The case is skipped only when the host cannot create directory
          symlinks. No work-path symlink is accepted, and every created link
          and target remains inside the temporary sandbox.
        """

        with synthetic_temporary_directory(
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

    def test_rejects_final_symlink_to_unrelated_directory(self) -> None:
        """@brief Reject a final work symlink without deleting its target.

        @return None after the unrelated target, marker, and link survive.
        @throws AssertionError If the helper accepts or mutates the synthetic
          symlink or target.
        @note The repository, symlink, and unrelated target are independent
          children of one disposable test-owned sandbox. The real checkout and
          its parents are never passed to the destructive helper.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-final-symlink-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            unrelated_target = sandbox / "unrelated-target"
            unrelated_target.mkdir()
            marker = unrelated_target / "must-survive"
            marker.write_text("unrelated", encoding="utf-8")
            work_link = sandbox / "work-link"
            try:
                work_link.symlink_to(
                    unrelated_target, target_is_directory=True
                )
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"directory symlinks unavailable: {error}")

            with self.assertRaises(ValueError) as raised:
                subject.remove_work_tree(work_link, synthetic_repo)

            self.assertIn(str(work_link), str(raised.exception))
            self.assertTrue(work_link.is_symlink())
            self.assertTrue(unrelated_target.is_dir())
            self.assertEqual(
                marker.read_text(encoding="utf-8"), "unrelated"
            )

    def test_rejects_symlinked_parent_of_unrelated_directory(self) -> None:
        """@brief Reject a symlinked work parent without deleting its target.

        @return None after the unrelated target tree, marker, and link survive.
        @throws AssertionError If the helper accepts or mutates the synthetic
          symlink component or target tree.
        @note The repository and unrelated target tree are separate children
          of one disposable test-owned sandbox. The real checkout and its
          parents are never passed to the destructive helper.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-parent-symlink-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            unrelated_parent = sandbox / "unrelated-parent"
            work_target = unrelated_parent / "work"
            work_target.mkdir(parents=True)
            marker = work_target / "must-survive"
            marker.write_text("unrelated", encoding="utf-8")
            parent_link = sandbox / "parent-link"
            try:
                parent_link.symlink_to(
                    unrelated_parent, target_is_directory=True
                )
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"directory symlinks unavailable: {error}")

            with self.assertRaises(ValueError) as raised:
                subject.remove_work_tree(
                    parent_link / "work", synthetic_repo
                )

            self.assertIn(str(parent_link), str(raised.exception))
            self.assertTrue(parent_link.is_symlink())
            self.assertTrue(work_target.is_dir())
            self.assertEqual(
                marker.read_text(encoding="utf-8"), "unrelated"
            )

    def test_propagates_removal_failure_and_checks_postcondition(self) -> None:
        """@brief Require deletion errors and silent no-op deletion to fail.

        @return None after both injected failure modes are observed.
        @throws AssertionError If an injected failure is hidden.
        @note The recursive remover is mocked, so the test never attempts to
          delete anything outside its synthetic work directory.
        """

        with synthetic_temporary_directory(
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

        with synthetic_temporary_directory(
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

        with synthetic_temporary_directory(
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

        with synthetic_temporary_directory(
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
