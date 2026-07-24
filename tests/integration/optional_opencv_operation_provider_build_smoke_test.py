#!/usr/bin/env python3
"""Deterministic safety regressions for the optional-provider build smoke."""

from __future__ import annotations

import json
import os
import pathlib
import stat
import tempfile
import unittest
from typing import Optional
from unittest import mock

import cmake_build_smoke_support as build_support
import image_artifact_codec_dependency_disabled_smoke as image_consumer
import optional_opencv_operation_provider_build_smoke as subject


#: @brief Stable disk-cache concurrency cases required in focused inventories.
#: @note These test-owned values independently mirror the CMake contract.
DISK_CACHE_CTEST_NAMES = (
    (
        "DiskCacheDiagnosticConcurrency."
        "RecordSnapshotClearAndPublicationRemainLive"
    ),
    (
        "DiskCacheDiagnosticConcurrency."
        "SameStoreAndOppositeDirectionExchangeRemainLive"
    ),
    "DiskCacheDiagnosticConcurrency.SnapshotBadAllocReleasesScopedGuard",
)
#: @brief Stable production lifecycle cases required in focused inventories.
#: @note The tuple intentionally contains exactly the two registered cases.
KERNEL_LIFECYCLE_CTEST_NAMES = (
    (
        "KernelLifecycleConcurrency."
        "ConcurrentPublicationListingAndCloseUseProductionObjects"
    ),
    (
        "KernelLifecycleConcurrency."
        "ShutdownAndGraphPublicationShareOneProductionAdmissionBoundary"
    ),
)
#: @brief Stable focused optional-provider case required by the nested profile.
#: @note This exact value keeps the regression independent of production sets.
OPTIONAL_PROVIDER_CTEST_NAME = (
    "OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores"
)
#: @brief Stable build-smoke entry required by every provider-disabled profile.
#: @note Its own build-smoke label is not inherited by diagnostic cases.
DEPENDENCY_DISABLED_CTEST_NAME = "DependencyDisabledInstallSmoke"


def ctest_json_test(
    name: str,
    *,
    labels: Optional[list[str]] = None,
    timeout: Optional[int] = None,
) -> dict[str, object]:
    """@brief Construct one synthetic CTest JSON-v1 test record.

    @param name Nonempty registered test name.
    @param labels Optional serialized CTest LABELS value.
    @param timeout Optional serialized CTest TIMEOUT value in seconds.
    @return Test record with a complete property list.
    @throws Nothing; callers provide deterministic in-memory values.
    @note The helper mirrors only fields consumed by the production parser.
    """

    properties: list[dict[str, object]] = []
    if labels is not None:
        properties.append({"name": "LABELS", "value": labels})
    if timeout is not None:
        properties.append({"name": "TIMEOUT", "value": timeout})
    return {"name": name, "properties": properties}


def provider_disabled_ctest_payload() -> str:
    """@brief Construct the valid provider-disabled JSON-v1 inventory.

    @return JSON payload containing two profile entries, three disk cases, and
      two production lifecycle cases.
    @throws Nothing; every serialized value is deterministic and JSON-safe.
    @note Disk cases receive a 20-second timeout; lifecycle cases receive a
      60-second timeout. Both groups use the exact `kernel-concurrency` label.
    """

    names = {
        DEPENDENCY_DISABLED_CTEST_NAME,
        OPTIONAL_PROVIDER_CTEST_NAME,
        *DISK_CACHE_CTEST_NAMES,
        *KERNEL_LIFECYCLE_CTEST_NAMES,
    }
    return json.dumps(
        {
            "tests": [
                ctest_json_test(
                    name,
                    labels=["kernel-concurrency"]
                    if name in DISK_CACHE_CTEST_NAMES
                    or name in KERNEL_LIFECYCLE_CTEST_NAMES
                    else None,
                    timeout=(
                        20
                        if name in DISK_CACHE_CTEST_NAMES
                        else (
                            60
                            if name in KERNEL_LIFECYCLE_CTEST_NAMES
                            else None
                        )
                    ),
                )
                for name in sorted(names)
            ]
        }
    )


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
                        build_support.remove_work_tree(
                            candidate, synthetic_repo
                        )
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
            with mock.patch.object(
                build_support.shutil, "rmtree"
            ) as remover:
                with self.assertRaisesRegex(ValueError, "parent traversal"):
                    build_support.resolve_work_directory(
                        traversal_work, synthetic_repo
                    )
                remover.assert_not_called()
            self.assertEqual(
                marker.read_text(encoding="utf-8"), "synthetic"
            )

    def test_rejects_empty_and_relative_work_spellings(self) -> None:
        """@brief Reject work spellings whose ownership root is ambiguous.

        @return None after empty and nonempty relative paths are rejected.
        @throws AssertionError If either spelling is accepted or recursive
          removal is reached.
        @note The candidates are parsed only; the mocked remover guarantees
          that this regression never performs recursive deletion.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-relative-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)

            for candidate in (pathlib.Path(), pathlib.Path("relative-work")):
                with self.subTest(candidate=candidate):
                    with mock.patch.object(
                        build_support.shutil, "rmtree"
                    ) as remover:
                        with self.assertRaisesRegex(
                            ValueError, "absolute"
                        ):
                            build_support.resolve_work_directory(
                                candidate, synthetic_repo
                            )
                        remover.assert_not_called()

    def test_accepts_only_injected_trusted_darwin_tmp_alias(self) -> None:
        """@brief Accept only the exact trusted Darwin temporary-root alias.

        The predicate is exercised with injected scalar filesystem facts, so
        this regression runs on every host without creating or replacing
        ``/tmp``. A synthetic trusted mapping then drives the real validation
        and recursive-removal path.

        @return None after the trusted child is removed through its physical
          spelling and every near-miss predicate is rejected.
        @throws OSError If the disposable synthetic tree cannot be created.
        @throws AssertionError If trust is widened, normalization escapes the
          physical subtree, or the logical root itself becomes removable.
        @note Injection is limited to private test helpers. Production callers
          always inspect the platform-owned ``/tmp`` and ``/private/tmp``.
        """

        physical_tmp = pathlib.Path("/private/tmp")
        self.assertIsNone(
            build_support._trusted_system_tmp_mapping(system_name="Linux")
        )
        self.assertTrue(
            build_support._is_trusted_darwin_tmp_alias(
                system_name="Darwin",
                alias_mode=stat.S_IFLNK | 0o777,
                alias_uid=0,
                resolved_alias=physical_tmp,
                physical_mode=stat.S_IFDIR | 0o1777,
                physical_uid=0,
            )
        )
        rejected_facts = (
            {"system_name": "Linux"},
            {"alias_mode": stat.S_IFDIR | 0o1777},
            {"alias_uid": 501},
            {"resolved_alias": pathlib.Path("/tmp-controlled")},
            {"physical_mode": stat.S_IFLNK | 0o777},
            {"physical_uid": 501},
        )
        trusted_facts = {
            "system_name": "Darwin",
            "alias_mode": stat.S_IFLNK | 0o777,
            "alias_uid": 0,
            "resolved_alias": physical_tmp,
            "physical_mode": stat.S_IFDIR | 0o1777,
            "physical_uid": 0,
        }
        for override in rejected_facts:
            with self.subTest(override=override):
                facts = dict(trusted_facts)
                facts.update(override)
                self.assertFalse(
                    build_support._is_trusted_darwin_tmp_alias(**facts)
                )

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-trusted-alias-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            logical_root = sandbox / "logical-tmp"
            logical_root.mkdir()
            physical_root = sandbox / "physical-tmp"
            physical_work = physical_root / "nested" / "work"
            physical_work.mkdir(parents=True)
            (physical_work / "stale").write_text(
                "stale", encoding="utf-8"
            )

            mapping = (logical_root, physical_root)
            with mock.patch.object(
                build_support,
                "_trusted_system_tmp_mapping",
                return_value=mapping,
            ):
                removed = build_support.remove_work_tree(
                    logical_root / "nested" / "work", synthetic_repo
                )
                with self.assertRaisesRegex(
                    ValueError, "temporary root"
                ):
                    build_support.resolve_work_directory(
                        logical_root, synthetic_repo
                    )

            self.assertEqual(removed, physical_work)
            self.assertFalse(physical_work.exists())
            self.assertTrue(logical_root.is_dir())
            self.assertTrue(physical_root.is_dir())

    def test_trusted_alias_still_rejects_remaining_symlink_component(
        self,
    ) -> None:
        """@brief Recheck untrusted components after trusted-prefix rewriting.

        @return None after a synthetic intermediate symlink and its unrelated
          target remain untouched.
        @throws OSError If the disposable tree cannot be created.
        @throws AssertionError If prefix trust suppresses later ``lstat``
          validation or mutates the unrelated target.
        @note The test injects only the logical-to-physical root mapping; the
          nested symlink is a real disposable filesystem object.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-work-alias-symlink-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            synthetic_repo = sandbox / "checkout" / "photospider"
            synthetic_repo.mkdir(parents=True)
            logical_root = sandbox / "logical-tmp"
            logical_root.mkdir()
            physical_root = sandbox / "physical-tmp"
            physical_root.mkdir()
            mapping = (logical_root, physical_root)
            unrelated_target = sandbox / "unrelated-target"
            unrelated_work = unrelated_target / "work"
            unrelated_work.mkdir(parents=True)
            marker = unrelated_work / "must-survive"
            marker.write_text("unrelated", encoding="utf-8")
            physical_link = physical_root / "nested-link"
            try:
                physical_link.symlink_to(
                    unrelated_target, target_is_directory=True
                )
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"directory symlinks unavailable: {error}")

            with mock.patch.object(
                build_support,
                "_trusted_system_tmp_mapping",
                return_value=mapping,
            ):
                with self.assertRaisesRegex(
                    ValueError, "symlink component"
                ):
                    build_support.remove_work_tree(
                        logical_root / "nested-link" / "work",
                        synthetic_repo,
                    )

            self.assertTrue(physical_link.is_symlink())
            self.assertEqual(
                marker.read_text(encoding="utf-8"), "unrelated"
            )

    def test_both_consumers_share_the_hardened_remover(self) -> None:
        """@brief Lock both destructive build-smoke consumers to one helper.

        @return None after both imported remover objects match the shared
          support implementation.
        @throws AssertionError If either consumer regains a divergent cleanup
          path.
        @note Import identity is the maintained seam: both real CTest drivers
          call the same validated remover before nested configuration.
        """

        self.assertIs(
            image_consumer.remove_work_tree, build_support.remove_work_tree
        )
        self.assertIs(subject.remove_work_tree, build_support.remove_work_tree)

    def test_rejects_current_filesystem_root_during_resolution(self) -> None:
        """@brief Reject the current filesystem root through its guard.

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
            with mock.patch.object(
                build_support.shutil, "rmtree"
            ) as remover:
                with self.assertRaisesRegex(ValueError, "filesystem root"):
                    build_support.resolve_work_directory(
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
                        build_support.remove_work_tree(
                            candidate, synthetic_repo
                        )
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
                build_support.remove_work_tree(work_link, synthetic_repo)

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
                build_support.remove_work_tree(
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
                build_support.shutil,
                "rmtree",
                side_effect=OSError("injected recursive removal failure"),
            ):
                with self.assertRaisesRegex(
                    OSError, "injected recursive removal failure"
                ):
                    build_support.remove_work_tree(work, synthetic_repo)
            self.assertTrue(work.is_dir())

            with mock.patch.object(
                build_support.shutil, "rmtree", return_value=None
            ):
                with self.assertRaisesRegex(
                    RuntimeError, "directory still exists"
                ):
                    build_support.remove_work_tree(work, synthetic_repo)
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

            resolved = build_support.remove_work_tree(work, synthetic_repo)

            self.assertEqual(resolved, work.resolve())
            self.assertFalse(work.exists())
            self.assertEqual(marker.read_text(encoding="utf-8"), "synthetic")


class ConfigurationLayoutTest(unittest.TestCase):
    """@brief Verifies cache-driven executable layout without running CMake.

    @throws AssertionError When cache interpretation contradicts generator mode.
    @note Every cache is synthetic and isolated under a temporary directory.
    """

    def test_resolves_single_and_multi_config_layouts_from_cache(self) -> None:
        """@brief Require cache metadata to choose executable subdirectories.

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


class ProviderDisabledProfileTest(unittest.TestCase):
    """@brief Verifies cache and CTest inventory profile contracts.

    @throws AssertionError When the validator accepts a mismatched capability
      profile or provider-dependent broad-suite inventory.
    @note Tests use only synthetic dictionaries and JSON; no CMake process or
      real build tree is accessed.
    """

    def test_accepts_exact_cache_and_rejects_provider_mismatch(self) -> None:
        """@brief Require the intended provider-off capability combination.

        @return None after the exact profile succeeds and an enabled provider
          is rejected.
        @throws AssertionError If either validation outcome is incorrect.
        @note Unrelated cache entries are allowed because generator/toolchain
          metadata is outside this profile contract.
        """

        values = {
            "BUILD_TESTING": "ON",
            "PHOTOSPIDER_ENABLE_OPENCV": "ON",
            "PHOTOSPIDER_ENABLE_YAML": "ON",
            "PHOTOSPIDER_BUILD_GRAPH_CLI": "ON",
            "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS": "ON",
            "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER": "OFF",
            "PHOTOSPIDER_BUILD_IPC": "OFF",
            "CMAKE_GENERATOR": "Synthetic",
        }
        subject.validate_provider_disabled_cache(values)

        values["PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER"] = "ON"
        with self.assertRaisesRegex(RuntimeError, "cache profile mismatch"):
            subject.validate_provider_disabled_cache(values)

    def test_accepts_exact_focused_ctest_inventory(self) -> None:
        """@brief Parse and accept the supported provider-off CTest surface.

        @return None after parsing preserves seven names and concurrency
          properties.
        @throws AssertionError If parsing or validation rejects the contract.
        @note Exact labels exclude the build-smoke label from disk test cases.
        """

        expected = {
            DEPENDENCY_DISABLED_CTEST_NAME,
            OPTIONAL_PROVIDER_CTEST_NAME,
            *DISK_CACHE_CTEST_NAMES,
            *KERNEL_LIFECYCLE_CTEST_NAMES,
        }

        inventory = subject.parse_ctest_inventory(
            provider_disabled_ctest_payload()
        )

        self.assertEqual(set(inventory), expected)
        subject.validate_provider_disabled_inventory(inventory)

    def test_rejects_malformed_broad_or_drifted_ctest_inventory(self) -> None:
        """@brief Reject malformed, broad, missing, or drifted inventories.

        @return None after every invalid inventory raises RuntimeError.
        @throws AssertionError If malformed or drifted inventory is accepted.
        @note Missing disk cases model the former full-suite-only registration;
          the broad example models an unbuilt scheduler discovery placeholder.
        """

        with self.assertRaisesRegex(RuntimeError, "no test list"):
            subject.parse_ctest_inventory("{}")
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            subject.parse_ctest_inventory(
                json.dumps(
                    {
                        "tests": [
                            ctest_json_test("duplicate"),
                            ctest_json_test("duplicate"),
                        ]
                    }
                )
            )

        old_full_only_inventory = {
            name: {}
            for name in {
                DEPENDENCY_DISABLED_CTEST_NAME,
                OPTIONAL_PROVIDER_CTEST_NAME,
            }
        }
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            subject.validate_provider_disabled_inventory(
                old_full_only_inventory
            )

        valid_inventory = subject.parse_ctest_inventory(
            provider_disabled_ctest_payload()
        )
        drifted_inventory = {
            name: dict(properties)
            for name, properties in valid_inventory.items()
        }
        drifted_inventory[DISK_CACHE_CTEST_NAMES[-1]]["LABELS"] = [
            "kernel-concurrency",
            "build-smoke",
        ]
        with self.assertRaisesRegex(RuntimeError, "property mismatch"):
            subject.validate_provider_disabled_inventory(drifted_inventory)

        drifted_lifecycle_inventory = {
            name: dict(properties)
            for name, properties in valid_inventory.items()
        }
        drifted_lifecycle_inventory[KERNEL_LIFECYCLE_CTEST_NAMES[-1]][
            "TIMEOUT"
        ] = 20
        with self.assertRaisesRegex(RuntimeError, "property mismatch"):
            subject.validate_provider_disabled_inventory(
                drifted_lifecycle_inventory
            )

        broad_inventory = {
            name: dict(properties)
            for name, properties in valid_inventory.items()
        }
        broad_inventory["test_scheduler_NOT_BUILT"] = {}
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            subject.validate_provider_disabled_inventory(broad_inventory)


if __name__ == "__main__":
    unittest.main()
