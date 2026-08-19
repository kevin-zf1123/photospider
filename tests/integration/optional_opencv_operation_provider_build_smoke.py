#!/usr/bin/env python3
"""Build and run the provider-disabled operation replacement profile."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import subprocess

from cmake_build_smoke_support import remove_work_tree


#: @brief Exact focused targets built by the provider-disabled nested profile.
#: @note `test_kernel_contracts` is the profile's only direct consumer of the
#:   internal test product when the dependency-gated full suite is unavailable.
PROVIDER_DISABLED_BUILD_TARGETS = (
    "test_optional_opencv_operation_provider",
    "test_cpu_dense_tensor_image_operation",
    "test_value_identity_across_dsos",
    "test_disk_cache_diagnostic_concurrency",
    "test_kernel_lifecycle_concurrency",
    "test_kernel_contracts",
)

#: @brief Build-relative directory containing CMake-owned validation manifests.
#: @note The directory is generated during configure and is never installed.
CI_INVENTORY_RELATIVE_DIRECTORY = pathlib.Path("generated/ci_inventory")
#: @brief Exact first record required by every registered-GTest TSV manifest.
#: @note No comment, blank, or repeated-header record is legal after this line.
REGISTERED_GTEST_TARGET_INVENTORY_HEADER = (
    "# target\tconfigured executable"
)
#: @brief Legal local executable-target spelling shared with the CMake writer.
#: @note Colons are excluded because root BUILDSYSTEM_TARGETS entries are local
#:   targets rather than imported or alias names.
REGISTERED_GTEST_TARGET_NAME_PATTERN = re.compile(r"[A-Za-z0-9_.+-]+")


def contains_forbidden_ascii_control(
    text: str, allowed_controls: str = ""
) -> bool:
    """@brief Detect ASCII controls that are not structural delimiters.

    @param text Exact manifest text or field to inspect.
    @param allowed_controls Control characters reserved by the current record
      format, normally LF and TAB while validating the complete TSV.
    @return True if text contains a disallowed C0 control or DEL; otherwise
      False.
    @throws None Character inspection is deterministic and in-memory.
    @note NUL is included even though CMake strings cannot represent it, so an
      externally modified or forged manifest still fails closed.
    """

    return any(
        character not in allowed_controls
        and (ord(character) < 32 or ord(character) == 127)
        for character in text
    )


def is_portable_absolute_path(path_text: str) -> bool:
    """@brief Recognize native absolute paths independent of the host OS.

    @param path_text One control-free executable-path TSV field.
    @return True for an absolute POSIX path, Windows drive-rooted path, or
      Windows UNC path; otherwise False.
    @throws None Pure path parsing performs no filesystem access.
    @note Backslashes are valid Windows path data. The caller later creates a
      host-native ``Path`` so real same-host manifests retain ``is_file``
      behavior, while cross-platform safety fixtures remain parseable.
    """

    return pathlib.PurePosixPath(path_text).is_absolute() or (
        pathlib.PureWindowsPath(path_text).is_absolute()
    )


def run(command: list[str], cwd: pathlib.Path) -> None:
    """@brief Run one required nested-build command with inherited output.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing working directory for the child process.
    @return None after the command exits successfully.
    @throws OSError If the child process cannot be started.
    @throws subprocess.CalledProcessError If the command exits nonzero.
    @note The complete command is printed before execution, and inherited
      standard streams remain visible to CTest.
    """

    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def cmake_cache_values(build: pathlib.Path) -> dict[str, str]:
    """@brief Read exact assignments from one nested CMake cache.

    @param build Configured nested build directory containing CMakeCache.txt.
    @return Mapping from cache keys to their final serialized values.
    @throws OSError If the cache cannot be read.
    @throws UnicodeError If the cache is not valid UTF-8 text.
    @throws RuntimeError If the nested build has no regular CMake cache file.
    @note Comment and malformed lines are ignored; later duplicate keys win,
      matching CMake's effective final assignment.
    """

    cache_path = build / "CMakeCache.txt"
    if not cache_path.is_file():
        raise RuntimeError(f"nested provider build has no cache: {cache_path}")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        assignment, value = line.split("=", 1)
        if ":" not in assignment:
            continue
        key, _cache_type = assignment.split(":", 1)
        if key:
            values[key] = value
    return values


def validate_provider_disabled_cache(values: dict[str, str]) -> None:
    """@brief Validate the exact supported provider-disabled test profile.

    @param values Effective assignments parsed from the nested CMake cache.
    @return None after every required capability and target choice matches.
    @throws RuntimeError If a required cache entry is missing or has an
      unexpected value.
    @note OpenCV, YAML, graph CLI, and operation plugins intentionally remain
      enabled; only the repository operation provider and IPC are disabled.
    """

    expected = {
        "BUILD_TESTING": "ON",
        "PHOTOSPIDER_ENABLE_OPENCV": "ON",
        "PHOTOSPIDER_ENABLE_YAML": "ON",
        "PHOTOSPIDER_BUILD_GRAPH_CLI": "ON",
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PLUGINS": "ON",
        "PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER": "OFF",
        "PHOTOSPIDER_BUILD_IPC": "OFF",
    }
    mismatches = {
        key: (expected_value, values.get(key))
        for key, expected_value in expected.items()
        if values.get(key) != expected_value
    }
    if mismatches:
        raise RuntimeError(
            "nested provider-disabled cache profile mismatch: "
            f"{mismatches}"
        )


def registered_gtest_target_files(
    build: pathlib.Path, configuration: str
) -> dict[str, pathlib.Path]:
    """@brief Read configured GoogleTest targets and executable locations.

    @param build Configured nested provider-disabled build directory.
    @param configuration Exact single- or multi-config name selected for the
      nested build.
    @return Mapping from every uniquely registered GoogleTest target to its
      configuration-specific executable path.
    @throws OSError If the generated inventory cannot be read.
    @throws UnicodeError If the generated inventory is not valid UTF-8.
    @throws RuntimeError If the configuration or inventory is missing,
      malformed, empty, duplicated, contains controls, lacks its exact unique
      header, or contains an invalid target or relative executable path.
    @note CMake generates the TSV from its ``gtest_discover_tests`` registration
      metadata and ``$<TARGET_FILE:...>`` expressions. Each later line is
      exactly one two-field data record; comments, repeated headers, and blank
      lines are rejected. POSIX absolute paths and Windows drive/UNC paths are
      accepted without rejecting ordinary spaces or Windows backslashes. This
      parser does not infer registrations from source filenames or from
      CTest's observed sentinel set.
    """

    if (
        not configuration
        or pathlib.PurePosixPath(configuration).name != configuration
        or pathlib.PureWindowsPath(configuration).name != configuration
        or configuration in {".", ".."}
        or contains_forbidden_ascii_control(configuration)
    ):
        raise RuntimeError(
            "invalid nested provider build configuration for GoogleTest "
            f"inventory: {configuration!r}"
        )
    inventory_path = (
        build
        / CI_INVENTORY_RELATIVE_DIRECTORY
        / f"registered_gtest_targets-{configuration}.tsv"
    )
    if not inventory_path.is_file():
        raise RuntimeError(
            "configured GoogleTest target inventory is missing: "
            f"{inventory_path}"
        )

    with inventory_path.open(
        "r", encoding="utf-8", newline=""
    ) as inventory_file:
        inventory_text = inventory_file.read()
    if contains_forbidden_ascii_control(inventory_text, "\n\t"):
        raise RuntimeError(
            "configured GoogleTest target inventory contains a forbidden "
            "ASCII control character"
        )
    lines = inventory_text.split("\n")
    if lines and not lines[-1]:
        lines.pop()
    if not lines or lines[0] != REGISTERED_GTEST_TARGET_INVENTORY_HEADER:
        raise RuntimeError(
            "configured GoogleTest target inventory has a missing or "
            "invalid header"
        )

    targets: dict[str, pathlib.Path] = {}
    for line_number, line in enumerate(lines[1:], start=2):
        if not line:
            raise RuntimeError(
                "configured GoogleTest target inventory contains a blank entry"
            )
        if line.startswith("#"):
            raise RuntimeError(
                "configured GoogleTest target inventory contains a comment "
                f"or repeated header at line {line_number}"
            )
        fields = line.split("\t")
        if len(fields) != 2:
            raise RuntimeError(
                "configured GoogleTest target inventory contains a malformed "
                f"entry at line {line_number}"
            )
        target_name, executable_text = fields
        executable = pathlib.Path(executable_text)
        if (
            REGISTERED_GTEST_TARGET_NAME_PATTERN.fullmatch(target_name) is None
            or target_name.endswith("_NOT_BUILT")
            or not is_portable_absolute_path(executable_text)
        ):
            raise RuntimeError(
                "configured GoogleTest target inventory contains an invalid "
                f"entry at line {line_number}"
            )
        if target_name in targets:
            raise RuntimeError(
                "configured GoogleTest target inventory contains a duplicate "
                f"target: {target_name!r}"
            )
        targets[target_name] = executable
    if not targets:
        raise RuntimeError("configured GoogleTest target inventory is empty")
    return targets


def expected_registered_gtest_sentinels(
    build: pathlib.Path, configuration: str
) -> set[str]:
    """@brief Derive exact registered-but-unbuilt CTest placeholders.

    @param build Configured and selectively built provider-disabled tree.
    @param configuration Exact configuration whose target paths were built.
    @return Sentinel names for registered GoogleTest executables that are not
      regular files after the focused build.
    @throws OSError If the configured target inventory cannot be read.
    @throws UnicodeError If the configured inventory is not valid UTF-8.
    @throws RuntimeError If the configured target inventory is invalid.
    @note Target-file existence observes the real build closure, including
      targets built indirectly as dependencies. The returned names follow
      CMake GoogleTest's exact ``${target}_NOT_BUILT`` convention.
    """

    targets = registered_gtest_target_files(build, configuration)
    return {
        f"{target_name}_NOT_BUILT"
        for target_name, executable in targets.items()
        if not executable.is_file()
    }


def parse_ctest_inventory(
    payload: str,
) -> dict[str, dict[str, object]]:
    """@brief Parse tests and properties from CTest's JSON inventory.

    @param payload Complete stdout from `ctest --show-only=json-v1`.
    @return Mapping from unique registered names to unique properties.
    @throws RuntimeError If the payload is invalid JSON, lacks a test list,
      contains a malformed test/property entry, or repeats a name/property.
    @note The parser consumes CTest's machine-readable schema rather than
      locale-sensitive human inventory text.
    """

    try:
        document = json.loads(payload)
    except json.JSONDecodeError as error:
        raise RuntimeError("CTest inventory is not valid JSON") from error
    tests = document.get("tests") if isinstance(document, dict) else None
    if not isinstance(tests, list):
        raise RuntimeError("CTest inventory has no test list")
    inventory: dict[str, dict[str, object]] = {}
    for test in tests:
        name = test.get("name") if isinstance(test, dict) else None
        if not isinstance(name, str) or not name:
            raise RuntimeError("CTest inventory contains a malformed test")
        if name in inventory:
            raise RuntimeError("CTest inventory contains duplicate test names")
        raw_properties = test.get("properties")
        if not isinstance(raw_properties, list):
            raise RuntimeError("CTest inventory test has no property list")
        properties: dict[str, object] = {}
        for raw_property in raw_properties:
            property_name = (
                raw_property.get("name")
                if isinstance(raw_property, dict)
                else None
            )
            if (
                not isinstance(property_name, str)
                or not property_name
                or "value" not in raw_property
            ):
                raise RuntimeError(
                    "CTest inventory contains a malformed test property"
                )
            if property_name in properties:
                raise RuntimeError(
                    "CTest inventory contains duplicate test properties"
                )
            properties[property_name] = raw_property["value"]
        inventory[name] = properties
    return inventory


def query_ctest_inventory(
    ctest_executable: str,
    build: pathlib.Path,
    configuration: str,
    cwd: pathlib.Path,
) -> dict[str, dict[str, object]]:
    """@brief Query one configured build's real CTest inventory.

    @param ctest_executable CTest executable paired with the selected CMake.
    @param build Configured nested provider-disabled build directory.
    @param configuration Exact build configuration to query.
    @param cwd Existing working directory for the child process.
    @return Registered tests and properties parsed from the JSON-v1 inventory.
    @throws OSError If CTest cannot be started.
    @throws subprocess.CalledProcessError If inventory discovery exits nonzero.
    @throws RuntimeError If the JSON inventory violates its expected schema.
    @note Captured stdout and stderr are echoed before validation so CTest
      retains complete nested-profile diagnostics.
    """

    command = [
        ctest_executable,
        "--test-dir",
        str(build),
        "--show-only=json-v1",
        "-C",
        configuration,
    ]
    print("+", " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.stdout:
        print(completed.stdout, end="", flush=True)
    if completed.stderr:
        print(completed.stderr, end="", flush=True)
    return parse_ctest_inventory(completed.stdout)


def validate_provider_disabled_inventory(
    inventory: dict[str, dict[str, object]],
    expected_sentinels: set[str],
    *,
    native_plugin_execution_supported: bool,
) -> None:
    """@brief Require the exact provider-disabled CTest surface.

    @param inventory Unique registered tests and properties from the nested
      build.
    @param expected_sentinels Exact registered-but-unbuilt names derived from
      CMake's target inventory and the completed focused build closure.
    @param native_plugin_execution_supported Whether the configured target
      platform registers native operation-DSO execution tests.
    @return None when only the intended focused tests, install smoke, and
      CMake-derived registered-only sentinels exist.
    @throws RuntimeError If a focused test is missing, a sentinel is malformed
      or has properties, a broad-suite test remains registered, or a required
      concurrency property drifts.
    @note `test_kernel_contracts` remains a buildable focused target for the
      separate injected-codec smoke but is deliberately not broadly discovered
      in this provider-disabled CTest inventory.
    @note The current V-13 closure deliberately leaves the compute-I/O and
      packed-FP4 behavior targets unbuilt. Their placeholders are observations
      derived from the configured target manifest, not hard-coded inputs.
    @note Darwin builds the optional provider executable for compile coverage
      but does not register its native operation-DSO execution case because
      every native plugin role fails closed on that platform.
    """

    disk_cache_tests = {
        (
            "DiskCacheDiagnosticConcurrency."
            "RecordSnapshotClearAndPublicationRemainLive"
        ),
        (
            "DiskCacheDiagnosticConcurrency."
            "SameStoreAndOppositeDirectionExchangeRemainLive"
        ),
        (
            "DiskCacheDiagnosticConcurrency."
            "SnapshotBadAllocReleasesScopedGuard"
        ),
    }
    lifecycle_tests = {
        (
            "KernelLifecycleConcurrency."
            "ConcurrentPublicationListingAndCloseUseProductionObjects"
        ),
        (
            "KernelLifecycleConcurrency."
            "ShutdownAndGraphPublicationShareOneProductionAdmissionBoundary"
        ),
    }
    dense_image_tests = {
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
            "ImageViewPreservesHugeZeroStrideExtentWithoutNarrowing"
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
            "FormalCommitPreservesValidatedOpaqueNativeImageValue"
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
            "DiskSaveUsesSealedValueAsSoleImageAuthority"
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
            "ImageRectHpPlanRejectsRouteSwitchBeforeTaskPopulation"
        ),
        (
            "CpuDenseTensorImageOperation."
            "ImageRectRtPlanRejectsRouteSwitchBeforeTaskPopulation"
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
    }
    value_runtime_tests = {
        "ValueIdentityAcrossDsos.MintingAuthorityIsProcessWide"
    }
    malformed_sentinels = sorted(
        name
        for name in expected_sentinels
        if re.fullmatch(r"[A-Za-z0-9_.:+-]+_NOT_BUILT", name) is None
    )
    if malformed_sentinels:
        raise RuntimeError(
            "provider-disabled expected sentinel inventory is malformed: "
            f"{malformed_sentinels}"
        )
    native_plugin_tests = set()
    if native_plugin_execution_supported:
        native_plugin_tests.add(
            "OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores"
        )
    expected = {"DependencyDisabledInstallSmoke"} | native_plugin_tests | (
        disk_cache_tests
        | lifecycle_tests
        | dense_image_tests
        | value_runtime_tests
        | expected_sentinels
    )
    names = set(inventory)
    if names != expected:
        raise RuntimeError(
            "provider-disabled CTest inventory mismatch: "
            f"expected {sorted(expected)}, got {sorted(names)}"
        )

    sentinel_property_mismatches = {
        name: {
            "LABELS": inventory[name].get("LABELS"),
            "TIMEOUT": inventory[name].get("TIMEOUT"),
        }
        for name in expected_sentinels
        if "LABELS" in inventory[name] or "TIMEOUT" in inventory[name]
    }
    if sentinel_property_mismatches:
        raise RuntimeError(
            "provider-disabled registered-only CTest property mismatch: "
            f"{sentinel_property_mismatches}"
        )

    expected_properties = {
        **{
            name: {"LABELS": ["kernel-concurrency"], "TIMEOUT": 20}
            for name in disk_cache_tests
        },
        **{
            name: {"LABELS": ["kernel-concurrency"], "TIMEOUT": 60}
            for name in lifecycle_tests
        },
        **{
            name: {"LABELS": ["value-runtime"], "TIMEOUT": 30}
            for name in value_runtime_tests
        },
    }
    property_mismatches = {
        name: {
            "LABELS": inventory[name].get("LABELS"),
            "TIMEOUT": inventory[name].get("TIMEOUT"),
        }
        for name, expected_property in expected_properties.items()
        if inventory[name].get("LABELS") != expected_property["LABELS"]
        or inventory[name].get("TIMEOUT") != expected_property["TIMEOUT"]
    }
    if property_mismatches:
        raise RuntimeError(
            "provider-disabled concurrency CTest property mismatch: "
            f"{property_mismatches}"
        )


def configured_test_executable(
    build: pathlib.Path, configuration: str
) -> pathlib.Path:
    """@brief Resolve the focused test path from nested generator state.

    @param build Configured nested build directory.
    @param configuration Exact configuration requested from the build tool.
    @return Expected focused test executable for the cached generator mode.
    @throws OSError If the nested CMake cache cannot be read.
    @throws UnicodeError If the nested CMake cache is not valid UTF-8 text.
    @throws RuntimeError If configuration metadata is missing or contradicts
      the requested configuration.
    @note A nonempty CMAKE_CONFIGURATION_TYPES value is the sole authority for
      multi-config layout. Platform identity affects only the executable
      suffix and never the presence of a configuration directory.
    """

    cache = cmake_cache_values(build)
    configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if configuration_types:
        available = [
            candidate.strip()
            for candidate in configuration_types.split(";")
            if candidate.strip()
        ]
        if not configuration:
            raise RuntimeError(
                "multi-config nested provider build requires --config"
            )
        if configuration not in available:
            raise RuntimeError(
                "nested provider build configuration mismatch: "
                f"requested {configuration}, available {available}"
            )
        output_directory = build / "tests" / configuration
    else:
        if "CMAKE_BUILD_TYPE" not in cache:
            raise RuntimeError(
                "single-config nested provider build cache is missing "
                "CMAKE_BUILD_TYPE"
            )
        build_type = cache["CMAKE_BUILD_TYPE"]
        if not build_type:
            raise RuntimeError(
                "single-config nested provider build has an empty "
                "CMAKE_BUILD_TYPE"
            )
        if build_type != configuration:
            raise RuntimeError(
                "nested provider build type mismatch: "
                f"requested {configuration}, got {build_type}"
            )
        output_directory = build / "tests"

    executable_suffix = ".exe" if os.name == "nt" else ""
    return output_directory / (
        "test_optional_opencv_operation_provider" + executable_suffix
    )


def main() -> int:
    """@brief Configure, build, and run the provider-disabled regression.

    @return Zero after the focused provider, Value-runtime, and concurrency
      cases succeed.
    @throws OSError If path handling or command startup fails.
    @throws SystemExit If command-line parsing rejects the invocation.
    @throws UnicodeError If the nested CMake cache is not valid UTF-8 text.
    @throws ValueError If the destructive work path is empty/relative,
      traverses parents, names a protected repository/filesystem/temporary
      root, or contains an untrusted symlink component.
    @throws RuntimeError If cleanup, cache metadata, configuration selection,
      or executable discovery violates the nested-build contract.
    @throws subprocess.CalledProcessError If configure, build, or test
      execution exits nonzero.
    @note The function removes only the validated caller-owned work tree before
      configuration. Darwin's exact root-owned ``/tmp -> /private/tmp`` system
      alias is normalized to its physical prefix; no other symlink is trusted.
      The function leaves the successful nested build available to CTest
      cleanup and writes no separate report or provenance artifact.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--cmake-executable", required=True)
    parser.add_argument("--ctest-executable", required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args()

    repo = args.repo.resolve()
    work = remove_work_tree(args.work, repo)
    configuration = args.config or "RelWithDebInfo"
    native_plugin_execution_supported = platform.system() == "Linux"

    focused_filter = (
        "^(DiskCacheDiagnosticConcurrency\\..*|"
        "CpuDenseTensorImageOperation\\..*|"
        "KernelLifecycleConcurrency\\..*|"
        "ValueIdentityAcrossDsos\\..*"
    )
    if native_plugin_execution_supported:
        focused_filter += (
            "|OptionalOpenCvOperationProvider\\."
            "ReplacementExecutesAndRestores"
        )
    focused_filter += ")$"
    run(
        [
            args.cmake_executable,
            "-S",
            str(repo),
            "-B",
            str(work),
            f"-DCMAKE_BUILD_TYPE={configuration}",
            "-DBUILD_TESTING=ON",
            "-DPHOTOSPIDER_BUILD_IPC=OFF",
            "-DPHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF",
        ],
        repo,
    )
    validate_provider_disabled_cache(cmake_cache_values(work))

    build_command = [
        args.cmake_executable,
        "--build",
        str(work),
        "--target",
        *PROVIDER_DISABLED_BUILD_TARGETS,
        "-j",
        "4",
    ]
    build_command.extend(["--config", configuration])
    run(build_command, repo)

    executable = configured_test_executable(work, configuration)
    if not executable.is_file():
        raise RuntimeError(
            "nested provider test executable is missing for cached "
            f"configuration: {executable}"
        )
    inventory = query_ctest_inventory(
        args.ctest_executable, work, configuration, repo
    )
    validate_provider_disabled_inventory(
        inventory,
        expected_registered_gtest_sentinels(work, configuration),
        native_plugin_execution_supported=native_plugin_execution_supported,
    )
    run(
        [
            args.ctest_executable,
            "--test-dir",
            str(work),
            "--output-on-failure",
            "-C",
            configuration,
            "-R",
            focused_filter,
        ],
        repo,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
