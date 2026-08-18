#!/usr/bin/env python3
"""Deterministic safety regressions for the optional-provider build smoke."""

from __future__ import annotations

import json
import os
import pathlib
import stat
import subprocess
import tempfile
import unittest
from typing import Optional
from unittest import mock

import cmake_build_smoke_support as build_support
import image_artifact_codec_dependency_disabled_smoke as image_consumer
import optional_opencv_operation_provider_build_smoke as subject


#: @brief Resolved repository root used to load the production CMake writer.
#: @note The path is immutable and never passed to destructive helpers.
REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
#: @brief Exact CMake module used by the root producer configuration.
#: @note The safety fixture includes this module directly, avoiding a duplicate
#:   test-only generator implementation.
CI_INVENTORY_CMAKE_MODULE_PATH = (
    REPOSITORY_ROOT / "cmake" / "PhotospiderCiInventory.cmake"
)
#: @brief CMake launcher for the production generator fixture.
#: @note CTest supplies CMAKE_COMMAND through the environment; direct unittest
#:   execution resolves the conventional cmake command through PATH.
CMAKE_EXECUTABLE = os.environ.get("PHOTOSPIDER_CMAKE_EXECUTABLE", "cmake")


def write_exact_text(path: pathlib.Path, text: str) -> None:
    """@brief Write one UTF-8 fixture without host newline conversion.

    @param path Existing-parent test-owned destination.
    @param text Exact CMake or manifest text to serialize.
    @return None after the file is closed successfully.
    @throws OSError If the fixture cannot be opened or written.
    @note Explicit ``newline=''`` preserves CR/LF injection cases on Python 3.9
      and later and keeps CMake fixture inputs byte-stable across host systems.
    """

    with path.open("w", encoding="utf-8", newline="") as output_file:
        output_file.write(text)


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
#: @brief Stable process-wide Value identity case required by the profile.
#: @note The fixture loads two independently linked Value-using DSOs.
VALUE_RUNTIME_CTEST_NAME = (
    "ValueIdentityAcrossDsos.MintingAuthorityIsProcessWide"
)
#: @brief Stable registered-only executor placeholder required by the profile.
#: @note The production executor still compiles through the nested product.
COMPUTE_IO_EXECUTOR_NOT_BUILT_CTEST_NAME = (
    "test_compute_io_executor_NOT_BUILT"
)
#: @brief Stable packed-FP4 placeholder required by the V-13 profile closure.
#: @note The dependency-disabled install smoke builds this target separately;
#:   the provider-disabled focused target closure intentionally does not.
PACKED_FP4_NOT_BUILT_CTEST_NAME = (
    "test_packed_fp4_dense_tensor_NOT_BUILT"
)
#: @brief Current exact derived sentinels for the V-13 focused build closure.
#: @note Production code derives these from the CMake target manifest; this
#:   tuple is only the independent current-profile oracle in the safety suite.
PROVIDER_DISABLED_EXPECTED_SENTINELS = (
    COMPUTE_IO_EXECUTOR_NOT_BUILT_CTEST_NAME,
    PACKED_FP4_NOT_BUILT_CTEST_NAME,
)
#: @brief Stable Value-backed dense-image cases required without the provider.
#: @note The names independently mirror the dedicated integration target.
CPU_DENSE_IMAGE_CTEST_NAMES = (
    (
        "CpuDenseTensorImageOperation."
        "ReadyFenceQueuesWaitsAndCancellationIsObserverLocal"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ReadyFenceRetainsFailureAndDroppedCompleterPublishesCancellation"
    ),
    (
        "CpuDenseTensorImageOperation."
        "PendingWaitRetainsSoleExecutorUntilCallbackCompletion"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TerminalWaitRetainsSoleExecutorUntilCallbackCompletion"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TransferRetainsSoleExecutorUntilDestinationCompletion"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ConcurrentWaitRegistrationAndPublicationDeliverExactlyOnce"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ConcurrentCancellationAndCallbackEntryHaveOneWinner"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ConcurrentTransferDestructionAndCallbackSettleDestination"
    ),
    (
        "CpuDenseTensorImageOperation."
        "PendingProducerMakesPayloadReadableOnlyAfterReady"
    ),
    (
        "CpuDenseTensorImageOperation."
        "FailedPendingValueRetainsMetadataAndRejectsPayloadAccess"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ExplicitTransferRunsOnlyAsQueuedFakeDeviceTask"
    ),
    (
        "CpuDenseTensorImageOperation."
        "CpuToMetalTransferUsesInjectedProviderAndRejectsHostRead"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ExternalTransferFailureIsTypedAndLaterTransferRecovers"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ChainedTransferReadinessQueuesDistinctLaterTaskWithoutBlocking"
    ),
    (
        "CpuDenseTensorImageOperation."
        "DestroyedTransferCancelsDestinationAndQueuedCallbackBecomesNoOp"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TransferPropagatesFailedAndCancelledSourcesWithoutPayloadAccess"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ValueRejectsMalformedFacetStrideAndEnvelope"
    ),
    (
        "CpuDenseTensorImageOperation."
        "SnapshotRejectsHugeZeroStrideImageBeforeIntNarrowing"
    ),
    (
        "CpuDenseTensorImageOperation."
        "SnapshotCompactsReverseBroadcastAndPlanarImageLayouts"
    ),
    (
        "CpuDenseTensorImageOperation."
        "BuilderScopesWriteAuthorityAndReadLeaseLifetime"
    ),
    (
        "CpuDenseTensorImageOperation."
        "AllocationIdentityValidityDoesNotQueryAllocationLiveness"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ImmutableSignedOffsetViewsShareAllocationAndMintRevisions"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ValueCopiesShareBytesAndViewsRetainLifetime"
    ),
    (
        "CpuDenseTensorImageOperation."
        "DenseTensorViewMovesPreserveSourceAndReplaceDestination"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ImageViewMovesPreserveSourceAndReplaceDestination"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ValueDeepCopiesLvaluePayloadShapeAndStrides"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ValueDeepCopiesRvaluePayloadBeforeSourceOwnerRetires"
    ),
    (
        "CpuDenseTensorImageOperation."
        "GenericFloatingMatrixPreservesChannelsLatentsStridesAndBoundaries"
    ),
    (
        "CpuDenseTensorImageOperation."
        "FormalCommitPublishesPendingNativeValueAfterReadyUnderExplicitPlan"
    ),
    (
        "CpuDenseTensorImageOperation."
        "FormalCommitPublishesValidatedOpaqueCompatibilityImageValue"
    ),
    (
        "CpuDenseTensorImageOperation."
        "FormalHpCachePreservesAliasesAndResealsDirtyAndReplacementBytes"
    ),
    (
        "CpuDenseTensorImageOperation."
        "PartialHpValidityRemovesDiskArtifactsAndCannotBeReused"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorDirtyStagingPublishesFreshIdentityAndExactRegionAtCommit"
    ),
    (
        "CpuDenseTensorImageOperation."
        "DiskReloadMintsFreshRuntimeIdentitiesWithoutChangingCachePath"
    ),
    (
        "CpuDenseTensorImageOperation."
        "DiskSaveRejectsCompatibilityStagingBesideSealedValue"
    ),
    (
        "CpuDenseTensorImageOperation."
        "DenseInvertInferencePreservesExactLogicalDescriptor"
    ),
    (
        "CpuDenseTensorImageOperation."
        "DenseRunnerConsumesSealedValueAndPublishesExactResultRevision"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ProductRegistryAndExecutorInvertPaddedMultiChannelInput"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ProductExecutorInvertsOnlySelectedPaddedImageRect"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ProductExecutorUsesNegativeOriginImageRectCoordinates"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ProductExecutorUsesAllRankFourTensorSliceAxes"
    ),
    (
        "CpuDenseTensorImageOperation."
        "ProductExecutorHandlesEmptyWholeAndRejectsRankMismatch"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorDirtyPlanExecutesRegisteredProductAndStagesExactValidity"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorTargetPlanRejectsPreferredRouteAddedBeforeTaskPopulation"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorAllExternallySatisfiedPlanIgnoresDeviceInventoryMutation"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorDirtySelectedCompleteCacheRejectsDeviceInventoryMutation"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorPartialActivePlanRejectsDeviceInventoryMutation"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorDirtyPlanRecomputesMissingAndPartialIntermediateParents"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorUpstreamPlanRejectsPreferredRouteAddedBeforeTaskPopulation"
    ),
    (
        "CpuDenseTensorImageOperation."
        "TensorDirtyUpdateMergesSelectedBytesIntoExistingFullOutput"
    ),
    (
        "CpuDenseTensorImageOperation."
        "FreshTensorPartialOutputBecomesReusableOnlyAfterWholeCommit"
    ),
    (
        "CpuDenseTensorImageOperation."
        "RunnerRejectsExecuteAccessBeyondFrozenGrantAsComputeError"
    ),
)


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


def provider_disabled_ctest_payload(
    sentinels: tuple[str, ...] = PROVIDER_DISABLED_EXPECTED_SENTINELS,
    *,
    include_native_plugin_test: bool = True,
) -> str:
    """@brief Construct the valid provider-disabled JSON-v1 inventory.

    @param sentinels Registered-but-unbuilt target names with no properties.
    @param include_native_plugin_test Whether to include the native operation
      provider execution case used by supported target platforms.
    @return JSON payload containing the dependency profile entry, optional
      native-provider case, 52 dense-image cases, one Value-runtime case,
      three disk cases, two production lifecycle cases, and requested derived
      sentinels.
    @throws Nothing; every serialized value is deterministic and JSON-safe.
    @note Disk cases receive a 20-second timeout; lifecycle cases receive a
      60-second timeout. Both groups use the exact `kernel-concurrency` label;
      every registered-only sentinel has no label or timeout.
    """

    names = {
        DEPENDENCY_DISABLED_CTEST_NAME,
        VALUE_RUNTIME_CTEST_NAME,
        *DISK_CACHE_CTEST_NAMES,
        *KERNEL_LIFECYCLE_CTEST_NAMES,
        *CPU_DENSE_IMAGE_CTEST_NAMES,
        *sentinels,
    }
    if include_native_plugin_test:
        names.add(OPTIONAL_PROVIDER_CTEST_NAME)
    return json.dumps(
        {
            "tests": [
                ctest_json_test(
                    name,
                    labels=(
                        ["kernel-concurrency"]
                        if name in DISK_CACHE_CTEST_NAMES
                        or name in KERNEL_LIFECYCLE_CTEST_NAMES
                        else (
                            ["value-runtime"]
                            if name == VALUE_RUNTIME_CTEST_NAME
                            else None
                        )
                    ),
                    timeout=(
                        20
                        if name in DISK_CACHE_CTEST_NAMES
                        else (
                            60
                            if name in KERNEL_LIFECYCLE_CTEST_NAMES
                            else (
                                30
                                if name == VALUE_RUNTIME_CTEST_NAME
                                else None
                            )
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

    @throws OSError If a synthetic manifest or compiler-free CMake generator
      fixture cannot be created or started.
    @throws subprocess.CalledProcessError If the valid production-generator
      fixture fails configuration.
    @throws AssertionError When the validator accepts a mismatched capability
      profile or provider-dependent broad-suite inventory.
    @note Tests use disposable dictionaries, JSON, manifests, and one
      ``project(... NONE)`` generator fixture. No compiler, product build,
      install, or generated executable is started.
    """

    def test_builds_exact_focused_targets_with_internal_seam_consumer(
        self,
    ) -> None:
        """@brief Pin the provider-disabled nested build target closure.

        @return None after the target tuple contains the sole focused
          internal-test-product consumer, leaves the executor behavior target
          registered-only, and contains no full-suite-only target.
        @throws AssertionError If the long-lived dependency-disabled profile
          silently loses its kernel contract build, rebuilds the executor
          behavior target, or widens into the full suite.
        @note The CMake configure remains authoritative for the direct-link
          closure; the nested product still compiles the executor
          implementation while this test protects the Python smoke's exercised
          surface.
        """

        self.assertEqual(
            subject.PROVIDER_DISABLED_BUILD_TARGETS,
            (
                "test_optional_opencv_operation_provider",
                "test_cpu_dense_tensor_image_operation",
                "test_value_identity_across_dsos",
                "test_disk_cache_diagnostic_concurrency",
                "test_kernel_lifecycle_concurrency",
                "test_kernel_contracts",
            ),
        )
        self.assertNotIn(
            "test_compute_service_split",
            subject.PROVIDER_DISABLED_BUILD_TARGETS,
        )
        self.assertNotIn(
            "test_compute_io_executor",
            subject.PROVIDER_DISABLED_BUILD_TARGETS,
        )

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

        @return None after parsing preserves 62 names and focused-test
          properties.
        @throws AssertionError If parsing or validation rejects the contract.
        @note Exact labels exclude the build-smoke label from disk and
          Value-runtime test cases; both derived sentinels remain unlabelled.
        """

        expected = {
            DEPENDENCY_DISABLED_CTEST_NAME,
            OPTIONAL_PROVIDER_CTEST_NAME,
            VALUE_RUNTIME_CTEST_NAME,
            *PROVIDER_DISABLED_EXPECTED_SENTINELS,
            *DISK_CACHE_CTEST_NAMES,
            *KERNEL_LIFECYCLE_CTEST_NAMES,
            *CPU_DENSE_IMAGE_CTEST_NAMES,
        }

        inventory = subject.parse_ctest_inventory(
            provider_disabled_ctest_payload()
        )

        self.assertEqual(len(CPU_DENSE_IMAGE_CTEST_NAMES), 52)
        self.assertEqual(len(expected), 62)
        self.assertEqual(set(inventory), expected)
        subject.validate_provider_disabled_inventory(
            inventory,
            set(PROVIDER_DISABLED_EXPECTED_SENTINELS),
            native_plugin_execution_supported=True,
        )

    def test_accepts_darwin_inventory_without_native_execution(self) -> None:
        """@brief Pin the Darwin provider-off compile-only registration.

        @return None after the Darwin inventory omits exactly the native DSO
          execution case and both cross-profile substitutions are rejected.
        @throws AssertionError If platform registration profiles are confused.
        @note The provider target is still part of the focused build tuple;
          this test governs only its CTest execution registration.
        """

        sentinels = set(PROVIDER_DISABLED_EXPECTED_SENTINELS)
        darwin_inventory = subject.parse_ctest_inventory(
            provider_disabled_ctest_payload(
                include_native_plugin_test=False
            )
        )
        self.assertEqual(len(darwin_inventory), 61)
        self.assertNotIn(OPTIONAL_PROVIDER_CTEST_NAME, darwin_inventory)
        subject.validate_provider_disabled_inventory(
            darwin_inventory,
            sentinels,
            native_plugin_execution_supported=False,
        )
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            subject.validate_provider_disabled_inventory(
                darwin_inventory,
                sentinels,
                native_plugin_execution_supported=True,
            )
        supported_inventory = subject.parse_ctest_inventory(
            provider_disabled_ctest_payload()
        )
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            subject.validate_provider_disabled_inventory(
                supported_inventory,
                sentinels,
                native_plugin_execution_supported=False,
            )

    def test_derives_registered_only_sentinels_from_target_manifest(
        self,
    ) -> None:
        """@brief Derive placeholders from registered target-file existence.

        @return None after one built target is excluded and two arbitrary
          unbuilt registered targets become exact sentinels.
        @throws OSError If the synthetic manifest or executable cannot be
          created.
        @throws AssertionError If parsing loses registrations, depends on a
          known future target name, or accepts malformed metadata.
        @note Every path belongs to a disposable synthetic build. The arbitrary
          future target proves derivation is registration-driven rather than a
          source-file or count special case.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-gtest-inventory-"
        ) as temporary:
            build = pathlib.Path(temporary) / "build"
            inventory_dir = build / subject.CI_INVENTORY_RELATIVE_DIRECTORY
            inventory_dir.mkdir(parents=True)
            executable_dir = build / "tests"
            executable_dir.mkdir()
            built_target = executable_dir / "test_built"
            built_target.write_text("synthetic executable", encoding="utf-8")
            unbuilt_target = executable_dir / "test_unbuilt"
            future_target = executable_dir / "test_arbitrary_future"
            manifest = inventory_dir / (
                "registered_gtest_targets-RelWithDebInfo.tsv"
            )
            write_exact_text(
                manifest,
                "# target\tconfigured executable\n"
                f"test_built\t{built_target}\n"
                f"test_unbuilt\t{unbuilt_target}\n"
                f"test_arbitrary_future\t{future_target}\n",
            )

            self.assertEqual(
                subject.registered_gtest_target_files(
                    build, "RelWithDebInfo"
                ),
                {
                    "test_arbitrary_future": future_target,
                    "test_built": built_target,
                    "test_unbuilt": unbuilt_target,
                },
            )
            sentinels = subject.expected_registered_gtest_sentinels(
                build, "RelWithDebInfo"
            )
            self.assertEqual(
                sentinels,
                {
                    "test_arbitrary_future_NOT_BUILT",
                    "test_unbuilt_NOT_BUILT",
                },
            )
            inventory = subject.parse_ctest_inventory(
                provider_disabled_ctest_payload(tuple(sorted(sentinels)))
            )
            subject.validate_provider_disabled_inventory(
                inventory,
                sentinels,
                native_plugin_execution_supported=True,
            )

            malformed_payloads = {
                "missing-header": f"test_built\t{built_target}\n",
                "wrong-header": (
                    "# wrong\tconfigured executable\n"
                    f"test_built\t{built_target}\n"
                ),
                "repeated-header": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test_built\t{built_target}\n"
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                ),
                "later-comment": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test_built\t{built_target}\n"
                    "# injected comment\n"
                ),
                "blank-line": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test_built\t{built_target}\n\n"
                ),
                "extra-field": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test_built\t{built_target}\textra\n"
                ),
                "relative-path": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    "test_relative\trelative/test_relative\n"
                ),
                "duplicate-target": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test_built\t{built_target}\n"
                    f"test_built\t{built_target}\n"
                ),
                "sentinel-target": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test_bad_NOT_BUILT\t{unbuilt_target}\n"
                ),
                "alias-target": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    f"test::alias\t{unbuilt_target}\n"
                ),
                "single-field": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    "blank-target\n"
                ),
                "delete-in-path": (
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    "test_delete\t/absolute/del\x7fpath\n"
                ),
            }
            malformed_payloads.update(
                {
                    f"c0-in-path-{control_code}": (
                        f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                        "test_control\t/absolute/control"
                        f"{chr(control_code)}path\n"
                    )
                    for control_code in range(32)
                }
            )
            for case_name, payload in malformed_payloads.items():
                with self.subTest(case=case_name):
                    write_exact_text(manifest, payload)
                    with self.assertRaisesRegex(
                        RuntimeError, "inventory"
                    ):
                        subject.registered_gtest_target_files(
                            build, "RelWithDebInfo"
                        )

    def test_accepts_portable_absolute_paths_per_configuration(self) -> None:
        """@brief Accept safe POSIX and Windows target-file spellings.

        @return None after independent Debug and RelWithDebInfo manifests accept
          POSIX paths with spaces, Windows drive paths with either separator,
          and a Windows UNC path.
        @throws OSError If a synthetic configuration manifest cannot be written.
        @throws AssertionError If a safe path is rejected, configurations bleed
          together, or a relative Windows path is classified as absolute.
        @note Cross-platform parsing is lexical. Only same-host paths reach the
          later ``is_file`` observation in the real smoke.
        """

        self.assertFalse(subject.is_portable_absolute_path("C:relative.exe"))
        portable_paths = {
            "test_posix_space": "/absolute/path with space/test executable",
            "test_windows_backslash": (
                r"C:\Program Files\Photospider\test_provider.exe"
            ),
            "test_windows_forward": (
                "D:/build with space/Photospider/test_provider.exe"
            ),
            "test_windows_unc": (
                r"\\server\build share\Photospider\test_provider.exe"
            ),
        }
        with synthetic_temporary_directory(
            prefix="photospider-provider-portable-paths-"
        ) as temporary:
            build = pathlib.Path(temporary) / "build"
            inventory_dir = build / subject.CI_INVENTORY_RELATIVE_DIRECTORY
            inventory_dir.mkdir(parents=True)
            for configuration in ("Debug", "RelWithDebInfo"):
                manifest = inventory_dir / (
                    f"registered_gtest_targets-{configuration}.tsv"
                )
                write_exact_text(
                    manifest,
                    f"{subject.REGISTERED_GTEST_TARGET_INVENTORY_HEADER}\n"
                    + "".join(
                        f"{target_name}\t{path_text}\n"
                        for target_name, path_text in portable_paths.items()
                    ),
                )
                parsed = subject.registered_gtest_target_files(
                    build, configuration
                )
                self.assertEqual(set(parsed), set(portable_paths))
                for target_name, path_text in portable_paths.items():
                    with self.subTest(
                        configuration=configuration,
                        target=target_name,
                    ):
                        self.assertTrue(
                            subject.is_portable_absolute_path(path_text)
                        )
                        self.assertEqual(
                            parsed[target_name], pathlib.Path(path_text)
                        )

    def test_cmake_generator_emits_strict_configuration_manifest(
        self,
    ) -> None:
        """@brief Round-trip the production CMake generator into the parser.

        @return None after the real helper preserves a legal local target name,
          ordinary path spaces, and the RelWithDebInfo ``$<CONFIG>`` output.
        @throws OSError If the synthetic CMake project cannot be written or the
          configured launcher cannot start.
        @throws subprocess.CalledProcessError If the valid fixture configure or
          generation step fails.
        @throws AssertionError If generated TSV content fails the production
          parser or an illegal CMake target name reaches generation.
        @note The fixture uses an imported executable and ``project(... NONE)``;
          it runs CMake generation without a compiler, build, or executable.
          Both build-type variables are set so single- and multi-config
          generators select the same isolated configuration.
        """

        with synthetic_temporary_directory(
            prefix="photospider-provider-cmake-inventory-"
        ) as temporary:
            sandbox = pathlib.Path(temporary)
            source = sandbox / "source"
            build = sandbox / "build"
            source.mkdir()
            configured_executable = (
                sandbox / "bin with space" / "test.generated+target"
            )
            configured_executable.parent.mkdir()
            write_exact_text(configured_executable, "synthetic executable")
            write_exact_text(
                source / "CMakeLists.txt",
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(PhotospiderCiInventoryFixture NONE)\n"
                f'include("{CI_INVENTORY_CMAKE_MODULE_PATH.as_posix()}")\n'
                "add_executable(test.generated+target IMPORTED GLOBAL)\n"
                "set_target_properties(test.generated+target PROPERTIES\n"
                f'  IMPORTED_LOCATION "{configured_executable.as_posix()}")\n'
                "set(PHOTOSPIDER_TEST_GTEST_TARGETS "
                "test.generated+target)\n"
                "photospider_generate_registered_gtest_target_inventory(\n"
                '  "${CMAKE_BINARY_DIR}/generated/ci_inventory/'
                'registered_gtest_targets-$<CONFIG>.tsv"\n'
                "  PHOTOSPIDER_TEST_GTEST_TARGETS)\n",
            )
            subprocess.run(
                [
                    CMAKE_EXECUTABLE,
                    "-S",
                    str(source),
                    "-B",
                    str(build),
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                subject.registered_gtest_target_files(
                    build, "RelWithDebInfo"
                ),
                {"test.generated+target": configured_executable},
            )

            rejected_script = sandbox / "reject_target_name.cmake"
            write_exact_text(
                rejected_script,
                "cmake_minimum_required(VERSION 3.16)\n"
                f'include("{CI_INVENTORY_CMAKE_MODULE_PATH.as_posix()}")\n'
                "set(PHOTOSPIDER_TEST_GTEST_TARGETS "
                '"${PHOTOSPIDER_TEST_TARGET_NAME}")\n'
                "photospider_generate_registered_gtest_target_inventory(\n"
                f'  "{(sandbox / "rejected.tsv").as_posix()}"\n'
                "  PHOTOSPIDER_TEST_GTEST_TARGETS)\n",
            )
            for invalid_target_name in (
                "test::alias",
                "test target",
                "test_target_NOT_BUILT",
                "test\ttarget",
                "test\ntarget",
                "test\x7ftarget",
            ):
                with self.subTest(target=repr(invalid_target_name)):
                    result = subprocess.run(
                        [
                            CMAKE_EXECUTABLE,
                            (
                                "-DPHOTOSPIDER_TEST_TARGET_NAME="
                                f"{invalid_target_name}"
                            ),
                            "-P",
                            str(rejected_script),
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertRegex(
                        result.stderr,
                        (
                            r"(?:forbidden ASCII control code|"
                            r"invalid CMake target name)"
                        ),
                    )
                    self.assertNotIn(invalid_target_name, result.stderr)

    def test_rejects_labelled_derived_sentinels(self) -> None:
        """@brief Reject a label attached to either derived sentinel.

        @return None after validation reports the injected property drift.
        @throws AssertionError If the exact provider-disabled contract accepts
          the sentinel with a CTest label.
        @note Each mutation starts from the complete valid 62-entry inventory
          and changes only one sentinel's `LABELS` property.
        """

        expected_sentinels = set(PROVIDER_DISABLED_EXPECTED_SENTINELS)
        for sentinel in PROVIDER_DISABLED_EXPECTED_SENTINELS:
            with self.subTest(sentinel=sentinel):
                inventory = subject.parse_ctest_inventory(
                    provider_disabled_ctest_payload()
                )
                inventory[sentinel]["LABELS"] = ["build-smoke"]
                with self.assertRaisesRegex(
                    RuntimeError, "registered-only CTest property mismatch"
                ):
                    subject.validate_provider_disabled_inventory(
                        inventory,
                        expected_sentinels,
                        native_plugin_execution_supported=True,
                    )

    def test_rejects_timed_derived_sentinels(self) -> None:
        """@brief Reject a timeout attached to either derived sentinel.

        @return None after validation reports the injected property drift.
        @throws AssertionError If the exact provider-disabled contract accepts
          the sentinel with a CTest timeout.
        @note Each mutation starts from the complete valid 62-entry inventory
          and changes only one sentinel's `TIMEOUT` property.
        """

        expected_sentinels = set(PROVIDER_DISABLED_EXPECTED_SENTINELS)
        for sentinel in PROVIDER_DISABLED_EXPECTED_SENTINELS:
            with self.subTest(sentinel=sentinel):
                inventory = subject.parse_ctest_inventory(
                    provider_disabled_ctest_payload()
                )
                inventory[sentinel]["TIMEOUT"] = 1
                with self.assertRaisesRegex(
                    RuntimeError, "registered-only CTest property mismatch"
                ):
                    subject.validate_provider_disabled_inventory(
                        inventory,
                        expected_sentinels,
                        native_plugin_execution_supported=True,
                    )

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

        expected_sentinels = set(PROVIDER_DISABLED_EXPECTED_SENTINELS)
        old_full_only_inventory = {
            name: {}
            for name in {
                DEPENDENCY_DISABLED_CTEST_NAME,
                OPTIONAL_PROVIDER_CTEST_NAME,
            }
        }
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            subject.validate_provider_disabled_inventory(
                old_full_only_inventory,
                expected_sentinels,
                native_plugin_execution_supported=True,
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
            subject.validate_provider_disabled_inventory(
                drifted_inventory,
                expected_sentinels,
                native_plugin_execution_supported=True,
            )

        drifted_lifecycle_inventory = {
            name: dict(properties)
            for name, properties in valid_inventory.items()
        }
        drifted_lifecycle_inventory[KERNEL_LIFECYCLE_CTEST_NAMES[-1]][
            "TIMEOUT"
        ] = 20
        with self.assertRaisesRegex(RuntimeError, "property mismatch"):
            subject.validate_provider_disabled_inventory(
                drifted_lifecycle_inventory,
                expected_sentinels,
                native_plugin_execution_supported=True,
            )

        broad_inventory = {
            name: dict(properties)
            for name, properties in valid_inventory.items()
        }
        broad_inventory["test_scheduler_NOT_BUILT"] = {}
        with self.assertRaisesRegex(RuntimeError, "inventory mismatch"):
            subject.validate_provider_disabled_inventory(
                broad_inventory,
                expected_sentinels,
                native_plugin_execution_supported=True,
            )


if __name__ == "__main__":
    unittest.main()
