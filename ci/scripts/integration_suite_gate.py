#!/usr/bin/env python3
"""Aggregate the exact shared integration DAG into one validated image digest.

The helper accepts only GitHub-owned job conclusions, the independently
verified image digest, and the protected build-smoke route digest through an
exact environment contract. Every maintained result must be literal
``success``. The attestation result is special only in that a trusted
publishing route requires ``success`` while a read-only route requires literal
``skipped``. Output is appended only after all checks pass.
"""

from __future__ import annotations

import argparse
import os
import re
import stat
import sys
from collections.abc import Mapping
from pathlib import Path


class SuiteGateError(ValueError):
    """Report a missing, skipped, failed, unknown, or unsafe gate input."""


REQUIRED_RESULTS: tuple[tuple[str, str], ...] = (
    ("identity-preflight", "CI_IDENTITY_RESULT"),
    ("integration-plan", "CI_PLAN_RESULT"),
    ("build-integrity-default", "CI_BUILD_RESULT"),
    ("build-smoke-control", "CI_BUILD_SMOKE_CONTROL_RESULT"),
    ("verify-targeted-artifacts", "CI_VERIFY_RESULT"),
    ("targeted-artifacts-ready", "CI_ARTIFACT_READY_RESULT"),
    ("full-ctest", "CI_CTEST_RESULT"),
    ("build-smoke", "CI_BUILD_SMOKE_RESULT"),
    ("producer-build-smoke", "CI_PRODUCER_BUILD_SMOKE_RESULT"),
    ("openexr-smoke", "CI_OPENEXR_RESULT"),
    ("scripted-cli", "CI_SCRIPTED_CLI_RESULT"),
    ("propagation-script", "CI_PROPAGATION_RESULT"),
    ("plugin-load", "CI_PLUGIN_RESULT"),
    ("execution-repeat", "CI_EXECUTION_RESULT"),
    ("installed-package-consumer", "CI_INSTALLED_PACKAGE_RESULT"),
    ("sanitizer-asan", "CI_ASAN_RESULT"),
    ("sanitizer-tsan", "CI_TSAN_RESULT"),
    ("fuzz-codecs", "CI_FUZZ_RESULT"),
    ("sanitizer-asan-darwin", "CI_DARWIN_ASAN_RESULT"),
    ("sanitizer-tsan-darwin", "CI_DARWIN_TSAN_RESULT"),
    ("fuzz-codecs-darwin", "CI_DARWIN_FUZZ_RESULT"),
)


def validate_gate(environment: Mapping[str, str]) -> str:
    """Validate every required result and return the canonical image digest.

    Args:
        environment: Exact GitHub job-result, attestation-mode, image digest,
            and protected route digest environment supplied by the workflow.

    Returns:
        The validated ``sha256:<64 lowercase hex>`` image digest.

    Raises:
        SuiteGateError: A required result is not literal ``success``, the
            publish/attestation pair is incoherent, or either digest is
            malformed.

    Note:
        ``failure``, ``failed``, ``cancelled``, ``skipped``, missing, and any
        future unknown GitHub conclusion all fail for ordinary required jobs.
    """
    for job_name, variable in REQUIRED_RESULTS:
        result = environment.get(variable)
        if result != "success":
            raise SuiteGateError(
                f"{job_name} concluded {result!r}; expected literal 'success'"
            )

    publish = environment.get("CI_PUBLISH_REUSABLE_ATTESTATIONS")
    attestation = environment.get("CI_ATTESTATION_RESULT")
    if publish == "true":
        expected_attestation = "success"
    elif publish == "false":
        expected_attestation = "skipped"
    else:
        raise SuiteGateError(
            "publish_reusable_attestations must be literal 'true' or 'false'"
        )
    if attestation != expected_attestation:
        raise SuiteGateError(
            "attest-targeted-artifacts concluded "
            f"{attestation!r}; expected {expected_attestation!r}"
        )

    route_digest = environment.get("CI_ROUTE_SHA256")
    if route_digest is None or re.fullmatch(
        r"[0-9a-f]{64}", route_digest
    ) is None:
        raise SuiteGateError(
            "shared suite did not retain one canonical build-smoke route digest"
        )

    digest = environment.get("CI_IMAGE_DIGEST")
    if digest is None or re.fullmatch(r"sha256:[0-9a-f]{64}", digest) is None:
        raise SuiteGateError("shared suite did not retain one canonical image digest")
    return digest


def append_validated_digest(output_path: Path, digest: str) -> None:
    """Append the validated digest to one retained regular GitHub output file.

    Args:
        output_path: Existing absolute GitHub step-output file.
        digest: Canonical digest returned by :func:`validate_gate`.

    Returns:
        None after the exact output record is appended and synchronized.

    Raises:
        SuiteGateError: The path is relative, absent, aliased, non-regular, or
            cannot be opened/written safely.

    Note:
        Darwin and Linux are the only maintained platforms, both exposing
        ``O_NOFOLLOW``. The same descriptor is checked, written, and fsynced.
    """
    if not output_path.is_absolute():
        raise SuiteGateError("GitHub output path must be absolute")
    flags = os.O_WRONLY | os.O_APPEND | os.O_CLOEXEC
    nofollow = getattr(os, "O_NOFOLLOW", 0)
    if nofollow == 0:
        raise SuiteGateError("platform cannot refuse output symlinks")
    try:
        descriptor = os.open(output_path, flags | nofollow)
    except OSError as error:
        raise SuiteGateError(f"cannot open GitHub output safely: {error}") from error
    try:
        mode = os.fstat(descriptor).st_mode
        if not stat.S_ISREG(mode):
            raise SuiteGateError("GitHub output is not a regular file")
        remaining = memoryview(
            f"validated_image_digest={digest}\n".encode("ascii")
        )
        while remaining:
            written = os.write(descriptor, remaining)
            if written <= 0:
                raise SuiteGateError("GitHub output write made no progress")
            remaining = remaining[written:]
        os.fsync(descriptor)
    except OSError as error:
        raise SuiteGateError(f"cannot append GitHub output: {error}") from error
    finally:
        os.close(descriptor)


def build_parser() -> argparse.ArgumentParser:
    """Build the one-command protected suite-gate CLI."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="existing absolute GitHub step-output file",
    )
    return parser


def main() -> int:
    """Validate the environment and append output only after complete success."""
    arguments = build_parser().parse_args()
    try:
        digest = validate_gate(os.environ)
        append_validated_digest(arguments.output, digest)
    except (SuiteGateError, OSError) as error:
        print(f"integration suite gate failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
