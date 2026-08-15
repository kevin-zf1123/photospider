#!/usr/bin/env python3
"""Generate a canonical signed plugin trust bundle for maintained tests."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
from pathlib import Path
from typing import Sequence


_PACKAGE_ID = re.compile(r"[0-9a-f]{32}")
_KINDS = {"isolated-runtime", "operation", "policy"}


def _parse_arguments() -> argparse.Namespace:
    """Parse the closed test-bundle generator command line."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--signature", type=Path, required=True)
    parser.add_argument("--bad-signature", type=Path, required=True)
    parser.add_argument("--trust-root", required=True)
    parser.add_argument(
        "--entry",
        action="append",
        nargs=4,
        metavar=("KIND", "PACKAGE_ID", "GENERATION", "ARTIFACT"),
        default=[],
    )
    return parser.parse_args()


def _canonical_entry(raw: list[str]) -> str:
    """Hash and serialize one strictly validated manifest entry.

    @param raw Closed ``kind/package/generation/artifact`` field sequence.
    @return Canonical signed-manifest row containing the artifact SHA-256.
    @throws ValueError If any identity field is unsupported or noncanonical.
    @throws OSError If the complete artifact cannot be read.
    @note Hashing occurs from one path read for maintained test setup; product
      admission independently reopens, hashes, and retains the exact object.
    """

    kind, package_id, generation_text, artifact_text = raw
    if kind not in _KINDS:
        raise ValueError(f"unsupported plugin kind: {kind}")
    if not _PACKAGE_ID.fullmatch(package_id) or int(package_id, 16) == 0:
        raise ValueError(f"invalid package id: {package_id}")
    if not generation_text.isascii() or not generation_text.isdecimal():
        raise ValueError(f"invalid generation: {generation_text}")
    generation = int(generation_text)
    if generation <= 0 or str(generation) != generation_text:
        raise ValueError(f"noncanonical generation: {generation_text}")
    artifact = Path(artifact_text)
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    return f"{kind} {package_id} {generation_text} {digest}"


def write_plugin_trust_bundle(
    *,
    openssl: str,
    private_key: Path,
    manifest: Path,
    signature: Path,
    bad_signature: Path | None,
    trust_root: str,
    entries: Sequence[Sequence[str]],
) -> None:
    """Write one canonical manifest and detached Ed25519 test signature.

    @param openssl Exact OpenSSL CLI selected by the caller.
    @param private_key Repository test-only Ed25519 private key.
    @param manifest Destination canonical manifest path.
    @param signature Destination lowercase-hex detached signature path.
    @param bad_signature Optional one-nibble-mutated negative-test signature.
    @param trust_root Canonical diagnostic root identifier.
    @param entries Closed artifact entries to hash and serialize.
    @return None after every requested output is durably closed by Python.
    @throws ValueError For invalid, duplicate, or noncanonical input rows.
    @throws OSError If an input/output file or OpenSSL process is unavailable.
    @throws subprocess.CalledProcessError If signing fails.
    @throws RuntimeError If OpenSSL does not return one Ed25519 signature.
    @note The private key and generated bundle are maintained test data only;
      no production code imports or invokes this helper.
    """

    canonical_entries = [
        _canonical_entry(list(entry)) for entry in entries
    ]
    rows = sorted(canonical_entries)
    if len(rows) != len(set(rows)):
        raise ValueError("duplicate test trust manifest entry")
    content_roles = [(row.split(" ", 1)[0], row.rsplit(" ", 1)[1])
                     for row in rows]
    if len(content_roles) != len(set(content_roles)):
        raise ValueError("duplicate test trust manifest content-role entry")
    manifest_bytes = "\n".join(
        [
            "photospider-plugin-trust-manifest-v1",
            f"trust-root {trust_root}",
            *rows,
            "",
        ]
    ).encode("ascii")
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_bytes(manifest_bytes)
    completed = subprocess.run(
        [
            openssl,
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            str(private_key),
            "-in",
            str(manifest),
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if len(completed.stdout) != 64:
        raise RuntimeError("Ed25519 test signature is not 64 bytes")
    signature_text = completed.stdout.hex()
    signature.write_text(signature_text + "\n", encoding="ascii")
    if bad_signature is not None:
        flipped = (
            ("1" if signature_text[0] == "0" else "0")
            + signature_text[1:]
        )
        bad_signature.write_text(flipped + "\n", encoding="ascii")


def main() -> int:
    """Generate one command-line-selected maintained test trust bundle.

    @return Zero after canonical generation and signing succeeds.
    @throws Exceptions from argument validation and
      :func:`write_plugin_trust_bundle` unchanged for CMake/CTest diagnostics.
    @note Command-line use always requests the mutated negative-test signature.
    """

    arguments = _parse_arguments()
    write_plugin_trust_bundle(
        openssl=arguments.openssl,
        private_key=arguments.private_key,
        manifest=arguments.manifest,
        signature=arguments.signature,
        bad_signature=arguments.bad_signature,
        trust_root=arguments.trust_root,
        entries=arguments.entry,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
