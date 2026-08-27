#!/usr/bin/env python3
"""Route raw CTest inventory only from a fresh protected control checkout.

The build producer uploads untrusted raw CTest and generated profile/role
bytes. This helper runs in a different job from the exact protected workflow
commit, validates those bytes and their candidate/workflow/image identities,
reconstructs the complete label-driven matrix, and applies the temporary
current-main routing lock. It is the sole workflow matrix authority.

Before targeted artifact attestation, the same protected implementation
remeasures the independently downloaded raw bundle and binds its exact CTest
and envelope digests to the downloaded route-control manifest. This lets the
targeted verifier compare ordinary coverage without accepting a producer
matrix or reopening a candidate parser.

The candidate checkout, protected control checkout, downloaded input, and
control-owned output must be disjoint. No candidate helper or routing lock is
imported, executed, or hash-authorized after candidate CMake has run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from typing import Any, Mapping, NamedTuple, Sequence


_SCRIPT_DIRECTORY = Path(__file__).resolve().parent
if str(_SCRIPT_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIRECTORY))

import build_smoke_inventory as inventory_reader  # noqa: E402
import ci_profile_manifest as profile_reader  # noqa: E402


class RoutingError(ValueError):
    """Report malformed, stale, aliased, or incomplete routing state."""


class _Measurement(NamedTuple):
    """Retain one regular file's exact bytes, digest, and descriptor size.

    Attributes:
        content: Bytes agreed by two complete reads of one descriptor.
        sha256: Lowercase SHA-256 of ``content``.
        size: Stable size measured from the retained descriptor.

    Note:
        Measurements live only for one control invocation. The resulting
        canonical control manifest is the durable authority.
    """

    content: bytes
    sha256: str
    size: int


_RAW_MANIFEST_NAME = "raw-inventory.manifest.json"
_RAW_CTEST_NAME = "ctest-info-v1.json"
_PROFILE_INPUT_NAMES = (
    "build_profile_matrix_v1.tsv",
    "build_profile_matrix_v1.tsv.sha256",
    "ci_security_roles_v1.tsv",
)
_CONTROL_SCHEMA = "photospider-build-smoke-control-v1"
_RAW_SCHEMA = "photospider-build-smoke-raw-inventory-v1"


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """Build one strict JSON object while rejecting duplicate members.

    Args:
        pairs: Ordered key/value pairs supplied by ``json.loads``.

    Returns:
        Newly owned mapping containing every unique member.

    Raises:
        RoutingError: A key occurs more than once.

    Note:
        Duplicate rejection prevents last-value-wins ambiguity in untrusted
        raw inventory and downloaded control manifests.
    """
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RoutingError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _canonical_bytes(value: Any, *, readable: bool = False) -> bytes:
    """Return the exact canonical JSON encoding used by this control plane.

    Args:
        value: JSON-compatible value to encode.
        readable: Whether to use the protected-lock two-space layout.

    Returns:
        ASCII JSON with sorted keys and one terminal LF.

    Note:
        Compact output is used for manifests and workflow matrices; readable
        output is reserved for the reviewed routing lock format.
    """
    options: dict[str, Any] = {"ensure_ascii": True, "sort_keys": True}
    if readable:
        options["indent"] = 2
    else:
        options["separators"] = (",", ":")
    return (json.dumps(value, **options) + "\n").encode("utf-8")


def _stable_identity(value: os.stat_result) -> tuple[int, ...]:
    """Return the metadata tuple whose drift invalidates a retained input.

    Args:
        value: Descriptor or pathname metadata from ``stat``/``lstat``.

    Returns:
        Device, inode, mode, link count, size, and nanosecond time identity.

    Note:
        Callers compare pathname and descriptor tuples before and after both
        complete reads; no tuple is persisted across processes.
    """
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_nlink,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def _measure_regular(path: Path) -> _Measurement:
    """Measure one regular non-link file through a retained safe descriptor.

    Args:
        path: Exact downloaded or protected input pathname.

    Returns:
        Exact bytes, SHA-256, and descriptor-derived size.

    Raises:
        RoutingError: Required Linux/Darwin flags are absent, the path is a
            link/special/missing file, pathname or descriptor metadata drifts,
            or the two complete descriptor reads differ.

    Note:
        ``O_NONBLOCK`` makes FIFO/device probes bounded before ``fstat`` rejects
        them. The pathname is rebound after open and after both reads.
    """
    required_flags = ("O_NOFOLLOW", "O_NONBLOCK", "O_CLOEXEC")
    missing = [
        name
        for name in required_flags
        if not isinstance(getattr(os, name, None), int)
    ]
    if missing:
        raise RoutingError(
            f"{path}: required safe-open flags are unavailable: "
            + ", ".join(missing)
        )
    flags = (
        os.O_RDONLY
        | getattr(os, "O_NOFOLLOW")
        | getattr(os, "O_NONBLOCK")
        | getattr(os, "O_CLOEXEC")
    )
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        initial = os.fstat(descriptor)
        if not stat.S_ISREG(initial.st_mode):
            raise RoutingError(f"routing input is not a regular file: {path}")

        def require_stable(phase: str) -> None:
            """Require pathname and descriptor to retain initial identity."""
            current = os.fstat(descriptor)
            try:
                pathname = path.lstat()
            except OSError as error:
                raise RoutingError(
                    f"routing input pathname disappeared {phase}: {path}: {error}"
                ) from error
            if (
                not stat.S_ISREG(pathname.st_mode)
                or _stable_identity(current) != _stable_identity(initial)
                or _stable_identity(pathname) != _stable_identity(initial)
            ):
                raise RoutingError(
                    f"routing input identity changed {phase}: {path}"
                )

        require_stable("after open")
        reads: list[bytes] = []
        for pass_number in (1, 2):
            os.lseek(descriptor, 0, os.SEEK_SET)
            chunks: list[bytes] = []
            byte_count = 0
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                chunks.append(chunk)
                byte_count += len(chunk)
            if byte_count != initial.st_size:
                raise RoutingError(
                    f"routing input size changed during read: {path}"
                )
            require_stable(f"after read {pass_number}")
            reads.append(b"".join(chunks))
        if reads[0] != reads[1]:
            raise RoutingError(
                f"routing input bytes changed between retained reads: {path}"
            )
        return _Measurement(
            content=reads[0],
            sha256=hashlib.sha256(reads[0]).hexdigest(),
            size=initial.st_size,
        )
    except RoutingError:
        raise
    except OSError as error:
        raise RoutingError(f"cannot measure routing input {path}: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _decode_json(
    measurement: _Measurement,
    context: str,
    *,
    canonical: bool,
    readable: bool = False,
) -> Any:
    """Decode retained unique-member JSON and optionally require exact bytes.

    Args:
        measurement: Bytes and digest retained from one safe descriptor.
        context: Stable path/role text for diagnostics.
        canonical: Whether decoded data must re-encode to the same bytes.
        readable: Whether canonical comparison uses the reviewed lock layout.

    Returns:
        Detached decoded JSON value.

    Raises:
        RoutingError: UTF-8, JSON, duplicate-member, or canonical-byte
            validation fails.
    """
    try:
        value = json.loads(
            measurement.content.decode("utf-8"),
            object_pairs_hook=_unique_object,
        )
    except (UnicodeError, json.JSONDecodeError, RoutingError) as error:
        raise RoutingError(f"cannot decode strict JSON {context}: {error}") from error
    if canonical and measurement.content != _canonical_bytes(
        value, readable=readable
    ):
        raise RoutingError(f"JSON input is not bytewise canonical: {context}")
    return value


def _require_identity(value: str, pattern: str, context: str) -> str:
    """Return one string matching an exact canonical identity pattern.

    Args:
        value: Untrusted candidate identity to validate.
        pattern: Full-match regular expression defining canonical syntax.
        context: Stable field name used in a failure diagnostic.

    Returns:
        The unchanged validated string.

    Raises:
        RoutingError: ``value`` is not a string or does not fully match.
    """
    if not isinstance(value, str) or re.fullmatch(pattern, value) is None:
        raise RoutingError(f"{context} is malformed")
    return value


def _real_directory(path: Path, context: str) -> Path:
    """Resolve one existing real non-link directory for an ownership boundary.

    Args:
        path: Directory pathname supplied by the workflow.
        context: Stable boundary name used in diagnostics.

    Returns:
        Strictly resolved canonical directory path.

    Raises:
        RoutingError: The path is absent, linked, non-directory, or cannot be
            resolved.
    """
    if not path.is_dir() or path.is_symlink():
        raise RoutingError(f"{context} is not a real directory: {path}")
    try:
        return path.resolve(strict=True)
    except OSError as error:
        raise RoutingError(f"cannot resolve {context}: {path}: {error}") from error


def _require_disjoint(paths: Mapping[str, Path]) -> dict[str, Path]:
    """Require every named directory boundary to be pairwise non-overlapping.

    Args:
        paths: Named existing real directories.

    Returns:
        Resolved path mapping.

    Raises:
        RoutingError: A directory aliases, contains, or is contained by another.
    """
    resolved = {
        name: _real_directory(path, name) for name, path in paths.items()
    }
    items = list(resolved.items())
    for index, (left_name, left) in enumerate(items):
        for right_name, right in items[index + 1 :]:
            if left == right or left in right.parents or right in left.parents:
                raise RoutingError(
                    f"routing boundaries overlap: {left_name}={left}, "
                    f"{right_name}={right}"
                )
    return resolved


def _git_head(root: Path, context: str) -> str:
    """Return one checkout's exact lowercase full ``HEAD`` commit.

    Args:
        root: Candidate or protected checkout root.
        context: Stable checkout role used in diagnostics.

    Returns:
        Full 40-lowercase-hex commit identity.

    Raises:
        RoutingError: Git cannot resolve one canonical commit.

    Note:
        ``rev-parse`` reads repository identity only; it executes no checked-out
        candidate helper or hook.
    """
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "HEAD^{commit}"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    value = completed.stdout.strip()
    if completed.returncode != 0 or re.fullmatch(r"[0-9a-f]{40}", value) is None:
        diagnostic = completed.stderr.strip()
        raise RoutingError(
            f"cannot resolve {context} HEAD"
            + (f": {diagnostic}" if diagnostic else "")
        )
    return value


def _validate_lock(lock: Any) -> dict[str, Any]:
    """Validate the temporary protected four-partition routing lock.

    Args:
        lock: Strict decoded protected lock value.

    Returns:
        The same mapping after exact schema, ordering, role, and overlap checks.

    Raises:
        RoutingError: Fields, schema, producer tests, dedicated roles, ordering,
            uniqueness, or supported role values differ.
    """
    if not isinstance(lock, dict) or set(lock) != {
        "dedicated_consumer_roles",
        "default_consumer_role",
        "producer_tests",
        "schema",
    }:
        raise RoutingError("build-smoke routing lock fields differ")
    if lock["schema"] != "photospider-build-smoke-routing-v3":
        raise RoutingError("unknown build-smoke routing schema")
    if lock["default_consumer_role"] != "ctest-control":
        raise RoutingError("unknown build-smoke consumer artifact role")
    producer_tests = lock["producer_tests"]
    if (
        not isinstance(producer_tests, list)
        or not producer_tests
        or not all(isinstance(item, str) and item for item in producer_tests)
        or producer_tests != sorted(producer_tests)
        or len(producer_tests) != len(set(producer_tests))
    ):
        raise RoutingError("producer_tests must be a nonempty sorted unique array")
    dedicated_roles = lock["dedicated_consumer_roles"]
    if (
        not isinstance(dedicated_roles, dict)
        or not dedicated_roles
        or list(dedicated_roles) != sorted(dedicated_roles)
        or not all(
            isinstance(test, str)
            and test
            and isinstance(role, str)
            and role in {"installed-package", "openexr-metadata"}
            for test, role in dedicated_roles.items()
        )
    ):
        raise RoutingError(
            "dedicated_consumer_roles must be a sorted supported-role map"
        )
    if set(producer_tests) & set(dedicated_roles):
        raise RoutingError("producer and dedicated build-smoke routes overlap")
    return lock


def partition(
    matrix: Any, lock: Any
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    """Partition one protected-reader matrix into four exhaustive job matrices.

    Args:
        matrix: Complete deterministic matrix reconstructed from raw CTest JSON.
        lock: Protected current-main routing lock.

    Returns:
        Control, installed-package, OpenEXR-metadata, and former producer-local
        matrices. The fourth matrix now runs in a downstream control consumer;
        the candidate build producer never selects or executes it.

    Raises:
        RoutingError: Lock, matrix, ordering, role, or coverage differs.
    """
    protected = _validate_lock(lock)
    if not isinstance(matrix, dict) or set(matrix) != {"include"}:
        raise RoutingError("build-smoke matrix fields differ")
    entries = matrix["include"]
    if not isinstance(entries, list) or not entries:
        raise RoutingError("build-smoke matrix is empty")
    producer_tests = protected["producer_tests"]
    dedicated_roles = protected["dedicated_consumer_roles"]
    seen: set[str] = set()
    consumers: list[dict[str, str]] = []
    installed: list[dict[str, str]] = []
    openexr: list[dict[str, str]] = []
    producer: list[dict[str, str]] = []
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"artifact", "test"}:
            raise RoutingError("build-smoke matrix entry fields differ")
        artifact = entry["artifact"]
        test = entry["test"]
        if (
            not isinstance(artifact, str)
            or not artifact
            or not isinstance(test, str)
            or not test
        ):
            raise RoutingError("build-smoke matrix entry is empty")
        if test in seen:
            raise RoutingError(f"duplicate build-smoke identity: {test}")
        seen.add(test)
        if test in producer_tests:
            producer.append(
                {
                    "artifact": artifact,
                    "artifact_role": protected["default_consumer_role"],
                    "test": test,
                }
            )
        elif test in dedicated_roles:
            role = dedicated_roles[test]
            destination = installed if role == "installed-package" else openexr
            destination.append(
                {"artifact": artifact, "artifact_role": role, "test": test}
            )
        else:
            consumers.append(
                {
                    "artifact": artifact,
                    "artifact_role": protected["default_consumer_role"],
                    "test": test,
                }
            )
    if [entry["test"] for entry in producer] != producer_tests:
        raise RoutingError(
            "protected producer tests differ from discovered CTest inventory"
        )
    if not consumers:
        raise RoutingError("routing removed every build-smoke consumer")
    expected_installed = [
        test for test, role in dedicated_roles.items() if role == "installed-package"
    ]
    expected_openexr = [
        test for test, role in dedicated_roles.items() if role == "openexr-metadata"
    ]
    if [entry["test"] for entry in installed] != expected_installed:
        raise RoutingError(
            "protected installed-package tests differ from discovered CTest inventory"
        )
    if [entry["test"] for entry in openexr] != expected_openexr:
        raise RoutingError(
            "protected OpenEXR metadata tests differ from discovered CTest inventory"
        )
    routed = {
        entry["test"]
        for group in (consumers, installed, openexr, producer)
        for entry in group
    }
    if routed != seen or sum(
        len(group) for group in (consumers, installed, openexr, producer)
    ) != len(seen):
        raise RoutingError("build-smoke routing is not disjoint and exhaustive")
    return (
        {"include": consumers},
        {"include": installed},
        {"include": openexr},
        {"include": producer},
    )


def route(
    matrix_path: Path, lock_path: Path
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    """Load two local test fixtures and apply :func:`partition` safely.

    Args:
        matrix_path: Canonical compact matrix fixture.
        lock_path: Canonical readable routing-lock fixture.

    Returns:
        Four disjoint matrices in control, installed, OpenEXR, and
        producer-designated order.

    Raises:
        RoutingError: Either file is unsafe/noncanonical or partitioning fails.

    Note:
        Production routing uses :func:`create_control`; this low-level boundary
        exists for durable partition behavior tests and has no workflow output.
    """
    matrix_measurement = _measure_regular(matrix_path)
    lock_measurement = _measure_regular(lock_path)
    matrix = _decode_json(
        matrix_measurement, str(matrix_path), canonical=True
    )
    lock = _decode_json(
        lock_measurement, str(lock_path), canonical=True, readable=True
    )
    return partition(matrix, lock)


def _validate_raw_bundle(
    raw_dir: Path,
    *,
    candidate_commit: str,
    workflow_commit: str,
    image_digest: str,
    profile: str,
) -> tuple[dict[str, _Measurement], str]:
    """Measure and validate one flat untrusted producer inventory bundle.

    Args:
        raw_dir: Downloaded artifact directory owned by the producer job.
        candidate_commit: Exact called-workflow candidate identity.
        workflow_commit: Exact protected evaluator identity.
        image_digest: Exact shared image digest.
        profile: Exact profile, currently ``default``.

    Returns:
        Measurements keyed by declared member and the raw envelope digest.

    Raises:
        RoutingError: Entries are missing, extra, linked, special, nested,
            malformed, noncanonical, duplicated, or identity-mismatched.
    """
    permitted = {_RAW_MANIFEST_NAME, _RAW_CTEST_NAME, *_PROFILE_INPUT_NAMES}
    names: list[str] = []
    try:
        with os.scandir(raw_dir) as entries:
            for entry in entries:
                names.append(entry.name)
                if entry.name not in permitted:
                    raise RoutingError(
                        f"undeclared raw routing member: {entry.name}"
                    )
                if not entry.is_file(follow_symlinks=False):
                    raise RoutingError(
                        f"raw routing member is linked, nested, or special: {entry.name}"
                    )
    except OSError as error:
        raise RoutingError(f"cannot enumerate raw routing input: {error}") from error
    names.sort()
    if len(names) != len(set(names)):
        raise RoutingError("raw routing members are not unique")
    if _RAW_MANIFEST_NAME not in names or _RAW_CTEST_NAME not in names:
        raise RoutingError("raw routing bundle lacks its manifest or CTest inventory")
    profile_names = set(names) & set(_PROFILE_INPUT_NAMES)
    if profile_names not in (set(), set(_PROFILE_INPUT_NAMES)):
        raise RoutingError("raw generated profile identity is partial")
    measurements = {name: _measure_regular(raw_dir / name) for name in names}
    manifest_measurement = measurements[_RAW_MANIFEST_NAME]
    manifest = _decode_json(
        manifest_measurement,
        str(raw_dir / _RAW_MANIFEST_NAME),
        canonical=True,
    )
    if not isinstance(manifest, dict) or set(manifest) != {
        "candidate_commit",
        "files",
        "image_digest",
        "profile",
        "schema",
        "workflow_commit",
    }:
        raise RoutingError("raw routing manifest fields differ")
    if manifest["schema"] != _RAW_SCHEMA:
        raise RoutingError("raw routing manifest schema differs")
    expected_identity = {
        "candidate_commit": candidate_commit,
        "workflow_commit": workflow_commit,
        "image_digest": image_digest,
        "profile": profile,
    }
    for field, expected in expected_identity.items():
        if manifest[field] != expected:
            raise RoutingError(f"raw routing {field} differs from workflow input")
    files = manifest["files"]
    if not isinstance(files, list):
        raise RoutingError("raw routing file inventory is not an array")
    declared_names: list[str] = []
    for record in files:
        if not isinstance(record, dict) or set(record) != {"path", "sha256", "size"}:
            raise RoutingError("raw routing file record fields differ")
        name = record["path"]
        if (
            not isinstance(name, str)
            or name not in permitted
            or name == _RAW_MANIFEST_NAME
        ):
            raise RoutingError("raw routing file path is undeclared")
        if (
            not isinstance(record["sha256"], str)
            or re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) is None
            or not isinstance(record["size"], int)
            or isinstance(record["size"], bool)
            or record["size"] < 0
        ):
            raise RoutingError(f"raw routing measurement is malformed: {name}")
        declared_names.append(name)
        measured = measurements.get(name)
        if measured is None or (
            measured.sha256 != record["sha256"]
            or measured.size != record["size"]
        ):
            raise RoutingError(f"raw routing measurement differs: {name}")
    if (
        declared_names != sorted(declared_names)
        or len(declared_names) != len(set(declared_names))
    ):
        raise RoutingError("raw routing file records are not sorted and unique")
    if set(declared_names) != set(names) - {_RAW_MANIFEST_NAME}:
        raise RoutingError("raw routing manifest does not cover the exact member set")
    return measurements, manifest_measurement.sha256


def _prepare_output(path: Path) -> Path:
    """Create one fresh mode-0700 real control-output directory.

    Args:
        path: Job-owned output directory that must not preexist.

    Returns:
        Strictly resolved newly created directory.

    Raises:
        RoutingError: Residual/link state exists, the parent is unsafe, or the
            directory cannot be created/resolved.

    Note:
        Individual files are still created through ``O_EXCL`` by
        :func:`_write_new`.
    """
    if path.exists() or path.is_symlink():
        raise RoutingError(f"routing control output contains residual state: {path}")
    parent = path.parent
    if not parent.is_dir() or parent.is_symlink():
        raise RoutingError(f"routing control output parent is unsafe: {parent}")
    path.mkdir(mode=0o700)
    return _real_directory(path, "routing control output")


def _write_new(path: Path, content: bytes, mode: int = 0o600) -> None:
    """Write one complete fresh regular file without following residual state.

    Args:
        path: Fresh output pathname below the control-owned directory.
        content: Complete immutable bytes to write.
        mode: Creation mode, normally private mode 0600.

    Returns:
        None after every byte is written and synchronized.

    Raises:
        RoutingError: Required flags are absent, residual/link state exists, or
            creation, write progress, or synchronization fails.
    """
    required = ("O_NOFOLLOW", "O_CLOEXEC")
    if any(not isinstance(getattr(os, name, None), int) for name in required):
        raise RoutingError("platform cannot create protected routing output safely")
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_NOFOLLOW")
        | getattr(os, "O_CLOEXEC")
    )
    descriptor = -1
    try:
        descriptor = os.open(path, flags, mode)
        written = 0
        while written < len(content):
            count = os.write(descriptor, content[written:])
            if count <= 0:
                raise RoutingError(f"routing output write made no progress: {path}")
            written += count
        os.fsync(descriptor)
    except OSError as error:
        raise RoutingError(f"cannot create routing output {path}: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _append_github_outputs(path: Path, values: Mapping[str, str]) -> None:
    """Append exact single-line control outputs through one safe descriptor.

    Args:
        path: Existing absolute GitHub step-output file.
        values: Canonical nonempty single-line name/value records.

    Returns:
        None after the entire payload is appended and synchronized.

    Raises:
        RoutingError: A name/value or path is unsafe, required flags are absent,
            the descriptor is non-regular, or writing makes no progress.

    Note:
        Values are validated before opening, so a failure cannot append a
        partial set of route outputs.
    """
    if not path.is_absolute():
        raise RoutingError("GitHub output path must be absolute")
    for key, value in values.items():
        if (
            re.fullmatch(r"[a-z][a-z0-9_]*", key) is None
            or not value
            or "\n" in value
            or "\r" in value
        ):
            raise RoutingError(f"GitHub routing output is malformed: {key}")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    cloexec = getattr(os, "O_CLOEXEC", None)
    if not isinstance(nofollow, int) or not isinstance(cloexec, int):
        raise RoutingError("platform cannot open GitHub routing output safely")
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_APPEND | nofollow | cloexec)
    except OSError as error:
        raise RoutingError(f"cannot open GitHub routing output: {error}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise RoutingError("GitHub routing output is not a regular file")
        payload = "".join(
            f"{key}={value}\n" for key, value in values.items()
        ).encode("utf-8")
        written = 0
        while written < len(payload):
            count = os.write(descriptor, payload[written:])
            if count <= 0:
                raise RoutingError("GitHub routing output write made no progress")
            written += count
        os.fsync(descriptor)
    except OSError as error:
        raise RoutingError(f"cannot append GitHub routing output: {error}") from error
    finally:
        os.close(descriptor)


def create_control(arguments: argparse.Namespace) -> dict[str, str]:
    """Create the sole canonical build-smoke routing authority.

    Args:
        arguments: Parsed ``control`` command paths and expected identities.

    Returns:
        Exact compact matrices plus matrix and route digests.

    Raises:
        RoutingError: Checkout, path, raw inventory, CTest, profile, lock,
            partition, output, or identity validation fails.
    """
    candidate_commit = _require_identity(
        arguments.candidate_commit, r"[0-9a-f]{40}", "candidate commit"
    )
    workflow_commit = _require_identity(
        arguments.workflow_commit, r"[0-9a-f]{40}", "workflow commit"
    )
    image_digest = _require_identity(
        arguments.image_digest, r"sha256:[0-9a-f]{64}", "image digest"
    )
    if arguments.profile != "default":
        raise RoutingError("protected build-smoke control requires profile default")
    boundaries = _require_disjoint(
        {
            "candidate checkout": arguments.candidate_root,
            "protected control checkout": arguments.control_root,
            "downloaded raw inventory": arguments.raw_dir,
        }
    )
    candidate_root = boundaries["candidate checkout"]
    control_root = boundaries["protected control checkout"]
    raw_dir = boundaries["downloaded raw inventory"]
    script_root = Path(__file__).resolve().parents[2]
    if script_root != control_root:
        raise RoutingError(
            "routing helper is not executing from the declared protected control checkout"
        )
    if _git_head(candidate_root, "candidate checkout") != candidate_commit:
        raise RoutingError("candidate checkout HEAD differs from expected commit")
    if _git_head(control_root, "protected control checkout") != workflow_commit:
        raise RoutingError("protected control HEAD differs from workflow commit")

    output_root = _prepare_output(arguments.output_dir)
    if any(
        output_root == boundary
        or output_root in boundary.parents
        or boundary in output_root.parents
        for boundary in boundaries.values()
    ):
        raise RoutingError("routing output overlaps an input or checkout boundary")
    measurements, raw_digest = _validate_raw_bundle(
        raw_dir,
        candidate_commit=candidate_commit,
        workflow_commit=workflow_commit,
        image_digest=image_digest,
        profile=arguments.profile,
    )
    profile_dir = output_root / "profile-inventory"
    profile_dir.mkdir(mode=0o700)
    for name in _PROFILE_INPUT_NAMES:
        measurement = measurements.get(name)
        if measurement is not None:
            _write_new(profile_dir / name, measurement.content)
    try:
        resolved_profile = profile_reader.resolve(
            candidate_root,
            profile_dir,
            control_root=control_root,
        )
    except Exception as error:
        raise RoutingError(f"raw profile identity is invalid: {error}") from error
    resolved_bytes = _canonical_bytes(resolved_profile)
    _write_new(profile_dir / "resolved-security-profiles.json", resolved_bytes)
    matrix_sha256 = resolved_profile.get("matrix_sha256")
    _require_identity(
        matrix_sha256, r"[0-9a-f]{64}", "resolved matrix digest"
    )

    try:
        inventory = inventory_reader.parse_inventory(
            measurements[_RAW_CTEST_NAME].content
        )
        build_smokes = inventory.build_smokes(inventory_reader.BUILD_SMOKE_LABEL)
        matrix_text, _ = inventory_reader.build_matrix(build_smokes)
    except Exception as error:
        raise RoutingError(f"raw CTest build-smoke inventory is invalid: {error}") from error
    matrix = json.loads(matrix_text, object_pairs_hook=_unique_object)
    lock_path = control_root / "ci/locks/build-smoke-routing.json"
    lock_measurement = _measure_regular(lock_path)
    lock = _decode_json(
        lock_measurement, str(lock_path), canonical=True, readable=True
    )
    consumers, dedicated, openexr, producer = partition(matrix, lock)
    routes = {
        "build_smoke_matrix": consumers,
        "dedicated_build_smoke_matrix": dedicated,
        "openexr_build_smoke_matrix": openexr,
        "producer_build_smoke_matrix": producer,
    }
    control_manifest = {
        "candidate_commit": candidate_commit,
        "ctest_inventory_sha256": measurements[_RAW_CTEST_NAME].sha256,
        "image_digest": image_digest,
        "matrix_sha256": matrix_sha256,
        "profile": arguments.profile,
        "raw_inventory_sha256": raw_digest,
        "routes": routes,
        "schema": _CONTROL_SCHEMA,
        "workflow_commit": workflow_commit,
    }
    control_bytes = _canonical_bytes(control_manifest)
    route_sha256 = hashlib.sha256(control_bytes).hexdigest()
    _write_new(output_root / "build-smoke-control.manifest.json", control_bytes)
    output_values: dict[str, str] = {
        key: json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        for key, value in routes.items()
    }
    output_values.update(
        {"matrix_sha256": matrix_sha256, "route_sha256": route_sha256}
    )
    for key, value in output_values.items():
        _write_new(
            output_root / f"{key}.txt", (value + "\n").encode("utf-8")
        )
    if arguments.github_output is not None:
        _append_github_outputs(arguments.github_output, output_values)
    return output_values


def validate_control_bundle(
    *,
    raw_dir: Path,
    manifest_path: Path,
    route_sha256: str,
    candidate_commit: str,
    workflow_commit: str,
    image_digest: str,
    profile: str,
    matrix_sha256: str,
) -> tuple[bytes, dict[str, Any]]:
    """Bind downloaded raw inventory to its protected control authority.

    Args:
        raw_dir: Exact flat untrusted producer bundle downloaded independently.
        manifest_path: Canonical protected-control manifest pathname.
        route_sha256: Expected digest emitted by the protected route job.
        candidate_commit: Exact called-workflow candidate commit.
        workflow_commit: Exact protected evaluator commit.
        image_digest: Exact digest-qualified shared image identity.
        profile: Exact build profile, currently ``default``.
        matrix_sha256: Exact resolved declarative matrix digest.

    Returns:
        Retained raw CTest bytes and the validated control manifest.

    Raises:
        RoutingError: Raw/control paths overlap, either retained bundle is
            malformed, an external identity differs, or the control manifest's
            raw/CTest digests do not bind the exact downloaded bytes.

    Note:
        This is the production cross-job boundary reused by targeted artifact
        coverage verification. It does not trust a producer-supplied matrix and
        does not execute candidate code.
    """
    boundaries = _require_disjoint(
        {
            "downloaded raw inventory": raw_dir,
            "downloaded routing control": manifest_path.parent,
        }
    )
    raw_root = boundaries["downloaded raw inventory"]
    control_root = boundaries["downloaded routing control"]
    canonical_manifest = control_root / manifest_path.name
    if canonical_manifest != manifest_path.resolve(strict=True):
        raise RoutingError("build-smoke control manifest path is aliased")
    _require_identity(route_sha256, r"[0-9a-f]{64}", "route digest")
    _require_identity(candidate_commit, r"[0-9a-f]{40}", "candidate commit")
    _require_identity(workflow_commit, r"[0-9a-f]{40}", "workflow commit")
    _require_identity(image_digest, r"sha256:[0-9a-f]{64}", "image digest")
    _require_identity(matrix_sha256, r"[0-9a-f]{64}", "matrix digest")
    if profile != "default":
        raise RoutingError("protected build-smoke verification requires profile default")

    measurements, raw_digest = _validate_raw_bundle(
        raw_root,
        candidate_commit=candidate_commit,
        workflow_commit=workflow_commit,
        image_digest=image_digest,
        profile=profile,
    )
    measurement = _measure_regular(canonical_manifest)
    if measurement.sha256 != route_sha256:
        raise RoutingError("build-smoke control manifest digest differs")
    manifest = _decode_json(
        measurement, str(canonical_manifest), canonical=True
    )
    if not isinstance(manifest, dict) or set(manifest) != {
        "candidate_commit",
        "ctest_inventory_sha256",
        "image_digest",
        "matrix_sha256",
        "profile",
        "raw_inventory_sha256",
        "routes",
        "schema",
        "workflow_commit",
    }:
        raise RoutingError("build-smoke control manifest fields differ")
    if manifest["schema"] != _CONTROL_SCHEMA:
        raise RoutingError("build-smoke control manifest schema differs")
    expected = {
        "candidate_commit": candidate_commit,
        "workflow_commit": workflow_commit,
        "image_digest": image_digest,
        "profile": profile,
        "matrix_sha256": matrix_sha256,
        "ctest_inventory_sha256": measurements[_RAW_CTEST_NAME].sha256,
        "raw_inventory_sha256": raw_digest,
    }
    for field, value in expected.items():
        if manifest[field] != value:
            raise RoutingError(f"build-smoke control {field} differs")
    for digest_field in (
        "ctest_inventory_sha256",
        "matrix_sha256",
        "raw_inventory_sha256",
    ):
        _require_identity(
            manifest[digest_field], r"[0-9a-f]{64}", digest_field
        )
    routes = manifest["routes"]
    if not isinstance(routes, dict) or set(routes) != {
        "build_smoke_matrix",
        "dedicated_build_smoke_matrix",
        "openexr_build_smoke_matrix",
        "producer_build_smoke_matrix",
    }:
        raise RoutingError("build-smoke control route set differs")
    for name, matrix in routes.items():
        if not isinstance(matrix, dict) or set(matrix) != {"include"}:
            raise RoutingError(f"build-smoke control matrix is malformed: {name}")
        if not isinstance(matrix["include"], list) or not matrix["include"]:
            raise RoutingError(f"build-smoke control matrix is empty: {name}")
    return measurements[_RAW_CTEST_NAME].content, manifest


def verify_control(arguments: argparse.Namespace) -> None:
    """Verify raw inventory plus control manifest against trusted job outputs.

    Args:
        arguments: Parsed raw directory, manifest path, and expected candidate,
            workflow, image, profile, matrix, and route identities.

    Returns:
        None after exact raw/control digest, schema, identity, and route-shape
        validation.

    Raises:
        RoutingError: Measurement, raw/control digest, field, identity, matrix,
            or route set differs from the protected job outputs.

    Note:
        This boundary runs before targeted artifact attestation. It consumes no
        candidate parser and writes no output. The production library function
        is also reused by ordinary-CTest cross-boundary verification.
    """
    validate_control_bundle(
        raw_dir=arguments.raw_dir,
        manifest_path=arguments.manifest,
        route_sha256=arguments.route_sha256,
        candidate_commit=arguments.candidate_commit,
        workflow_commit=arguments.workflow_commit,
        image_digest=arguments.image_digest,
        profile=arguments.profile,
        matrix_sha256=arguments.matrix_sha256,
    )


def build_parser() -> argparse.ArgumentParser:
    """Build the protected control and downloaded-manifest verifier CLI.

    Returns:
        Parser with mutually exclusive ``control`` and ``verify-control``
        commands and their exact required identities.

    Note:
        Parsing performs no filesystem access; handlers own all safety checks.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    control = subparsers.add_parser(
        "control", help="derive all routing outputs from raw untrusted inventory"
    )
    control.add_argument("--raw-dir", type=Path, required=True)
    control.add_argument("--candidate-root", type=Path, required=True)
    control.add_argument("--control-root", type=Path, required=True)
    control.add_argument("--output-dir", type=Path, required=True)
    control.add_argument("--candidate-commit", required=True)
    control.add_argument("--workflow-commit", required=True)
    control.add_argument("--image-digest", required=True)
    control.add_argument("--profile", required=True)
    control.add_argument("--github-output", type=Path)
    control.set_defaults(handler=create_control)

    verify = subparsers.add_parser(
        "verify-control", help="verify a downloaded control manifest and digest"
    )
    verify.add_argument("--raw-dir", type=Path, required=True)
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--route-sha256", required=True)
    verify.add_argument("--candidate-commit", required=True)
    verify.add_argument("--workflow-commit", required=True)
    verify.add_argument("--image-digest", required=True)
    verify.add_argument("--profile", required=True)
    verify.add_argument("--matrix-sha256", required=True)
    verify.set_defaults(handler=verify_control)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Execute one protected routing control command and report failures.

    Args:
        argv: Optional explicit argument sequence for focused tests; ``None``
            consumes the process command line.

    Returns:
        Zero on complete control/verification success, otherwise one after a
        concise fail-closed diagnostic.

    Note:
        Parsed handler exceptions never produce a success code or traceback.
    """
    arguments = build_parser().parse_args(
        sys.argv[1:] if argv is None else argv
    )
    try:
        arguments.handler(arguments)
    except (OSError, RoutingError) as error:
        print(f"build-smoke routing failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
