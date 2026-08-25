#!/usr/bin/env python3
"""Durable regressions for protected CI identity and fail-closed readers."""

from __future__ import annotations

import gzip
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "ci/scripts"
COMMIT_A = subprocess.run(
    ["git", "-C", str(REPO_ROOT), "rev-parse", "--verify", "HEAD^{commit}"],
    check=True,
    text=True,
    capture_output=True,
).stdout.strip()
COMMIT_B = "2" * 40
IMAGE_DIGEST = "sha256:" + "3" * 64


def run_command(
    *arguments: object,
    expect_success: bool = True,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run one command at repository root and assert its expected outcome."""
    completed = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=REPO_ROOT,
        env={**os.environ, **(environment or {})},
        text=True,
        capture_output=True,
        check=False,
    )
    if expect_success and completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(map(str, arguments))}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if not expect_success and completed.returncode == 0:
        raise AssertionError(f"command unexpectedly passed: {' '.join(map(str, arguments))}")
    return completed


class ImageManifestContractTest(unittest.TestCase):
    """Exercise canonical image-input creation and exact OCI-label binding."""

    def test_manifest_and_labels_bind_exact_inputs(self) -> None:
        """Accept exact manifest/labels and reject a mismatched manifest label."""
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            manifest = temporary / "manifest.json"
            digest_sidecar = temporary / "manifest.sha256"
            created = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "create",
                "--source-commit", COMMIT_A,
                "--repository", "kevin-zf1123/photospider",
                "--output", manifest,
                "--digest-output", digest_sidecar,
            )
            digest = created.stdout.strip()
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            self.assertEqual(digest_sidecar.read_text(encoding="utf-8"), digest + "\n")
            run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "verify",
                "--source-commit", COMMIT_A,
                "--repository", "kevin-zf1123/photospider",
                "--manifest", manifest,
                "--expected-digest", digest,
            )
            labels = temporary / "labels.json"
            labels.write_text(
                json.dumps(
                    {
                        "org.opencontainers.image.revision": COMMIT_A,
                        "org.photospider.ci.input-manifest-sha256": digest,
                    },
                    sort_keys=True,
                ) + "\n",
                encoding="utf-8",
            )
            run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "verify-labels",
                "--manifest", manifest,
                "--labels-json", labels,
                "--source-commit", COMMIT_A,
            )
            labels.write_text(
                json.dumps(
                    {
                        "org.opencontainers.image.revision": COMMIT_A,
                        "org.photospider.ci.input-manifest-sha256": "0" * 64,
                    },
                    sort_keys=True,
                ) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "verify-labels",
                "--manifest", manifest,
                "--labels-json", labels,
                "--source-commit", COMMIT_A,
                expect_success=False,
            )
            self.assertIn("expected", failed.stderr)

    def test_publisher_rejects_head_after_last_image_input_commit(self) -> None:
        """Reject a later manual/push HEAD before registry publication can begin."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text) / "repository"
            lock = root / "ci/locks/ci-image-lock.json"
            lock.parent.mkdir(parents=True)
            lock.write_text(
                json.dumps({"input_paths": ["Dockerfile.ci"]}, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            (root / "Dockerfile.ci").write_text("FROM scratch\n", encoding="utf-8")
            run_command("git", "-C", root, "init", "-q")
            run_command("git", "-C", root, "config", "user.name", "CI Contract")
            run_command("git", "-C", root, "config", "user.email", "ci@example.invalid")
            run_command("git", "-C", root, "config", "commit.gpgsign", "false")
            run_command("git", "-C", root, "add", "--", "Dockerfile.ci", "ci/locks/ci-image-lock.json")
            run_command("git", "-C", root, "commit", "-q", "-m", "image input")
            image_source = run_command("git", "-C", root, "rev-parse", "HEAD").stdout.strip()
            accepted = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py",
                "--repo-root", root,
                "publish-source-commit",
                "--workflow-commit", image_source,
            )
            self.assertEqual(accepted.stdout.strip(), image_source)

            (root / "README.md").write_text("later unrelated commit\n", encoding="utf-8")
            run_command("git", "-C", root, "add", "--", "README.md")
            run_command("git", "-C", root, "commit", "-q", "-m", "unrelated")
            later_head = run_command("git", "-C", root, "rev-parse", "HEAD").stdout.strip()
            self.assertNotEqual(later_head, image_source)
            failed = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py",
                "--repo-root", root,
                "publish-source-commit",
                "--workflow-commit", later_head,
                expect_success=False,
            )
            self.assertIn("differs from workflow commit", failed.stderr)
            self.assertIn("refusing an unattestable publication", failed.stderr)

        workflow = (REPO_ROOT / ".github/workflows/build-ci-image.yml").read_text(
            encoding="utf-8"
        )
        guard = workflow.index("publish-source-commit")
        self.assertLess(guard, workflow.index("uses: docker/login-action@"))
        self.assertLess(guard, workflow.index("uses: docker/build-push-action@"))
        self.assertNotIn('--source-commit "${{ github.sha }}"', workflow)

    def test_published_identity_requires_attestation_before_output(self) -> None:
        """Exercise the shell resolver with exact mocked registry and gh boundaries."""
        with tempfile.TemporaryDirectory() as temporary_text:
            temporary = Path(temporary_text)
            source = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "source-commit"
            ).stdout.strip()
            manifest = temporary / "precomputed.json"
            digest = run_command(
                "python3", SCRIPTS / "ci_image_manifest.py", "create",
                "--source-commit", source,
                "--repository", "kevin-zf1123/photospider",
                "--output", manifest,
            ).stdout.strip()
            binary_dir = temporary / "bin"
            binary_dir.mkdir()
            docker_log = temporary / "docker.log"
            gh_log = temporary / "gh.log"
            docker = binary_dir / "docker"
            docker.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_DOCKER_LOG\"; printf '\\n' >> \"$CI_TEST_DOCKER_LOG\"\n"
                "case \"${1:-} ${2:-} ${3:-}\" in\n"
                "  'buildx imagetools inspect') printf 'Name: test\\nDigest: %s\\n' \"$CI_TEST_IMAGE_DIGEST\" ;;\n"
                "  'image inspect --format') printf '{\"org.opencontainers.image.revision\":\"%s\",\"org.photospider.ci.input-manifest-sha256\":\"%s\"}\\n' \"$CI_TEST_SOURCE_COMMIT\" \"$CI_TEST_MANIFEST_DIGEST\" ;;\n"
                "  *) exit 0 ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            docker.chmod(0o755)
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            python = binary_dir / "python3"
            python.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "if [[ ${1:-} == */ci_runner_verify.py ]]; then printf '{\"platform\":\"Linux\"}\\n'; exit 0; fi\n"
                f"exec {sys.executable!s} \"$@\"\n",
                encoding="utf-8",
            )
            python.chmod(0o755)
            output = temporary / "identity.env"
            image_digest = "sha256:" + "4" * 64
            environment = {
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "CI_ARTIFACT_DIR": str(temporary / "artifacts"),
                "CI_IMAGE_OUTPUT_FILE": str(output),
                "CI_IMAGE_REPOSITORY": "kevin-zf1123/photospider",
                "CI_TEST_DOCKER_LOG": str(docker_log),
                "CI_TEST_GH_LOG": str(gh_log),
                "CI_TEST_IMAGE_DIGEST": image_digest,
                "CI_TEST_MANIFEST_DIGEST": digest,
                "CI_TEST_SOURCE_COMMIT": source,
            }
            run_command("bash", SCRIPTS / "ci_image_verify.sh", environment=environment)
            outputs = output.read_text(encoding="utf-8")
            self.assertIn(f"digest={image_digest}", outputs)
            self.assertIn(
                f"image=ghcr.io/kevin-zf1123/photospider/photospider-ci@{image_digest}",
                outputs,
            )
            gh_arguments = gh_log.read_text(encoding="utf-8")
            self.assertIn("attestation verify", gh_arguments)
            self.assertIn("--deny-self-hosted-runners", gh_arguments)
            self.assertIn(source, gh_arguments)


class CommandTimeoutContractTest(unittest.TestCase):
    """Exercise the portable total-deadline primitive used by fuzz profiles."""

    def test_deadline_terminates_a_real_process_group(self) -> None:
        """Return 124 promptly when a live child exceeds its declared bound."""
        started = time.monotonic()
        failed = run_command(
            "python3", SCRIPTS / "ci_command_timeout.py",
            "--timeout-seconds", "1",
            "--label", "fuzz-contract-test",
            "--", sys.executable, "-c", "import time; time.sleep(30)",
            expect_success=False,
        )
        elapsed = time.monotonic() - started
        self.assertEqual(failed.returncode, 124)
        self.assertLess(elapsed, 8)
        self.assertIn("exceeded its declared 1-second job timeout", failed.stderr)

    def test_fuzz_runner_wraps_configure_build_and_keeps_input_timeout(self) -> None:
        """Bind the matrix job bound before work while retaining libFuzzer timeout."""
        runner = (SCRIPTS / "fuzz_smoke.sh").read_text(encoding="utf-8")
        wrapper = runner.index('exec python3 "$SCRIPT_DIR/ci_command_timeout.py"')
        self.assertLess(wrapper, runner.index("run_logged configure_fuzz_codecs"))
        self.assertIn('--timeout-minutes "$job_timeout_minutes"', runner)
        self.assertIn('-- bash "$SCRIPT_DIR/fuzz_smoke.sh" --bounded-worker', runner)
        self.assertIn('"-timeout=$timeout"', runner)


class RunnerIdentityContractTest(unittest.TestCase):
    """Exercise exact hosted-runner image and architecture readback."""

    def test_linux_runner_exact_identity_passes_and_rotation_fails(self) -> None:
        """Accept the protected image version and reject one rotated value."""
        module_path = SCRIPTS / "ci_runner_verify.py"
        specification = importlib.util.spec_from_file_location("ci_runner_verify_test", module_path)
        self.assertIsNotNone(specification)
        self.assertIsNotNone(specification.loader)
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        exact_environment = {"ImageOS": "ubuntu24", "ImageVersion": "20260816.277.1"}
        with (
            mock.patch.object(module.platform, "system", return_value="Linux"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
        ):
            verified = module.verify(REPO_ROOT, "Linux", "ubuntu-24.04")
        self.assertEqual(verified["runner_label"], "ubuntu-24.04")
        with (
            mock.patch.object(module.platform, "system", return_value="Linux"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(
                module.os.environ,
                {"ImageOS": "ubuntu24", "ImageVersion": "rotated"},
                clear=False,
            ),
            self.assertRaises(module.RunnerError),
        ):
            module.verify(REPO_ROOT, "Linux", "ubuntu-24.04")

        with (
            mock.patch.object(module.platform, "system", return_value="Linux"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
            self.assertRaises(module.RunnerError),
        ):
            module.verify(REPO_ROOT, "Linux", "ubuntu-latest")

    def test_darwin_runner_uses_one_arm64_label_and_triplet_identity(self) -> None:
        """Accept macos-15 arm64 and reject the former x86_64 interpretation."""
        module_path = SCRIPTS / "ci_runner_verify.py"
        specification = importlib.util.spec_from_file_location("ci_runner_verify_darwin_test", module_path)
        self.assertIsNotNone(specification)
        self.assertIsNotNone(specification.loader)
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        exact_environment = {"ImageOS": "macos15", "ImageVersion": "20260727.0256.1"}
        with (
            mock.patch.object(module.platform, "system", return_value="Darwin"),
            mock.patch.object(module.platform, "machine", return_value="arm64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
        ):
            verified = module.verify(REPO_ROOT, "Darwin", "macos-15")
        self.assertEqual(verified["architecture"], "arm64")
        with (
            mock.patch.object(module.platform, "system", return_value="Darwin"),
            mock.patch.object(module.platform, "machine", return_value="x86_64"),
            mock.patch.dict(module.os.environ, exact_environment, clear=False),
            self.assertRaises(module.RunnerError),
        ):
            module.verify(REPO_ROOT, "Darwin", "macos-15")


class ProfileReaderContractTest(unittest.TestCase):
    """Exercise exact fallback, complete versioned input, and partial rejection."""

    @staticmethod
    def _write_complete_versioned_identity(inventory: Path) -> str:
        """Write one canonical complete versioned security identity fixture.

        Args:
            inventory: Existing directory that will receive all three identity
                files.

        Returns:
            The SHA-256 digest binding the matrix and security-role records.
        """
        matrix_records = {
            ("capability", "address-sanitizer"): {
                "default": False,
                "dependencies": [],
                "option": "USE_ASAN",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "build-fuzzers"): {
                "default": False,
                "dependencies": [],
                "option": "PHOTOSPIDER_BUILD_FUZZERS",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "build-testing"): {
                "default": True,
                "dependencies": [],
                "option": "BUILD_TESTING",
                "platforms": ["Darwin", "Linux"],
            },
            ("capability", "thread-sanitizer"): {
                "default": False,
                "dependencies": [],
                "option": "USE_TSAN",
                "platforms": ["Darwin", "Linux"],
            },
            ("component", "security-validation"): {
                "dependencies": ["openssl", "utf8proc"],
                "targets": ["fuzz_codec", "test_contract"],
            },
            ("dependency", "openssl"): {
                "cmake_package": "OpenSSL",
                "lock": "ci/locks/requirements-ci.txt",
                "mode": "required",
                "vcpkg_port": "openssl",
            },
            ("dependency", "utf8proc"): {
                "cmake_package": "utf8proc",
                "lock": "ci/locks/requirements-ci.txt",
                "mode": "required",
                "vcpkg_port": "utf8proc",
            },
            ("profile", "fuzz-codecs"): {
                "cmake_args": [
                    "-DBUILD_TESTING=OFF",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=OFF",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["fuzz-artifact", "fuzz-codec"],
            },
            ("profile", "sanitizer-asan"): {
                "cmake_args": [
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=OFF",
                    "-DUSE_ASAN=ON",
                    "-DUSE_TSAN=OFF",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["sanitizer-artifact", "sanitizer-target", "security-label"],
            },
            ("profile", "sanitizer-tsan"): {
                "cmake_args": [
                    "-DBUILD_TESTING=ON",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=OFF",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=ON",
                ],
                "components": ["security-validation"],
                "dependencies": ["openssl", "utf8proc"],
                "platforms": ["Darwin", "Linux"],
                "roles": ["sanitizer-artifact", "sanitizer-target", "security-label"],
            },
            ("role", "fuzz-artifact"): {"kind": "artifact", "selector": "fuzz-smoke"},
            ("role", "fuzz-codec"): {
                "corpus_sha256": None,
                "job_timeout_minutes": 20,
                "kind": "fuzz-target",
                "max_len": 65536,
                "production_limit": 65536,
                "production_limit_symbol": "ps::codec::kLimit",
                "runs": 1000,
                "seed": 1,
                "selector": "fuzz_codec",
                "timeout_seconds": 2,
            },
            ("role", "sanitizer-artifact"): {
                "kind": "artifact",
                "selector": "sanitizer-smoke",
            },
            ("role", "sanitizer-target"): {"kind": "target", "selector": "test_contract"},
            ("role", "security-label"): {"kind": "ctest-label", "selector": "security"},
            ("target", "fuzz_codec"): {
                "capabilities": ["build-fuzzers"],
                "component": "security-validation",
            },
            ("target", "test_contract"): {
                "capabilities": ["build-testing"],
                "component": "security-validation",
            },
        }
        encoded_records = [
            f"{kind}\t{identifier}\t"
            + json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
            for (kind, identifier), value in matrix_records.items()
        ]
        matrix = inventory / "build_profile_matrix_v1.tsv"
        matrix.write_text(
            "schema\tphotospider-build-profile-matrix-v1\n"
            + "\n".join(sorted(encoded_records))
            + "\n",
            encoding="utf-8",
        )
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n",
            encoding="utf-8",
        )
        records = [
            "fuzz\tfuzz-codecs\tfuzz-codec",
            "sanitizer\tsanitizer-asan\t[\"sanitizer-target\"]\t[\"security-label\"]",
            "sanitizer\tsanitizer-tsan\t[\"sanitizer-target\"]\t[\"security-label\"]",
        ]
        (inventory / "ci_security_roles_v1.tsv").write_text(
            "schema\tphotospider-ci-security-roles-v1\n"
            f"matrix_sha256\t{digest}\n"
            + "\n".join(sorted(records)) + "\n",
            encoding="utf-8",
        )
        return digest

    @staticmethod
    def _refresh_versioned_digest(inventory: Path) -> str:
        """Rebind sidecar and roles header after one deliberate matrix mutation."""
        matrix = inventory / "build_profile_matrix_v1.tsv"
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n", encoding="utf-8"
        )
        roles = inventory / "ci_security_roles_v1.tsv"
        lines = roles.read_text(encoding="utf-8").splitlines()
        lines[1] = f"matrix_sha256\t{digest}"
        roles.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return digest

    def test_current_main_fallback_is_hash_bound(self) -> None:
        """Resolve current main only through the explicitly identified fallback."""
        completed = run_command(
            "python3", SCRIPTS / "ci_profile_manifest.py",
            "--profile", "fuzz-codecs",
        )
        resolved = json.loads(completed.stdout)
        self.assertTrue(resolved["fallback"])
        self.assertEqual(resolved["profile"]["seed"], 1)
        self.assertEqual(resolved["profile"]["runs"], 1000)
        self.assertEqual(
            [target["max_len"] for target in resolved["profile"]["targets"]],
            [65536, 131072],
        )

    def test_partial_versioned_identity_fails_closed(self) -> None:
        """Reject any subset of the three required versioned identity files."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            (inventory / "build_profile_matrix_v1.tsv").write_text(
                "schema\tphotospider-build-profile-matrix-v1\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("partial versioned profile identity", failed.stderr)

    def test_staging_clears_complete_stale_identity_before_fallback(self) -> None:
        """Never reuse a valid old output set when the current source is empty."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "empty-source"
            output = root / "artifact"
            source.mkdir()
            output.mkdir()
            self._write_complete_versioned_identity(output)
            resolved_path = output / "resolved-security-profiles.json"
            before = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", output,
                "--output", resolved_path,
            )
            self.assertEqual(before.returncode, 0)
            self.assertFalse(json.loads(resolved_path.read_text(encoding="utf-8"))["fallback"])

            run_command(
                "bash", SCRIPTS / "ci_profile_inventory_stage.sh", source, output,
            )
            for identity_name in (
                "build_profile_matrix_v1.tsv",
                "build_profile_matrix_v1.tsv.sha256",
                "ci_security_roles_v1.tsv",
            ):
                self.assertFalse((output / identity_name).exists())
            resolved = json.loads(resolved_path.read_text(encoding="utf-8"))
            self.assertTrue(resolved["fallback"])
            self.assertEqual(resolved["schema"], "photospider-resolved-ci-security-v1")

    def test_staging_rejects_symlink_and_special_output_targets(self) -> None:
        """Fail before clearing when an exact output name is not a regular file."""
        for unsafe_kind in ("symlink", "fifo"):
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                source = root / "source"
                output = root / "output"
                source.mkdir()
                output.mkdir()
                unsafe = output / "resolved-security-profiles.json"
                if unsafe_kind == "symlink":
                    outside = root / "outside.json"
                    outside.write_text("preserve\n", encoding="utf-8")
                    unsafe.symlink_to(outside)
                else:
                    os.mkfifo(unsafe)
                failed = run_command(
                    "bash", SCRIPTS / "ci_profile_inventory_stage.sh", source, output,
                    expect_success=False,
                )
                self.assertIn("Unsafe existing staged profile artifact", failed.stderr)

    def test_fallback_rejects_changed_candidate_authority(self) -> None:
        """Reject the temporary lists as soon as one hash-bound source changes."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
            lock_target = root / "ci/locks/current-main-profiles-v1.json"
            lock_target.parent.mkdir(parents=True)
            shutil.copyfile(lock_source, lock_target)
            lock = json.loads(lock_source.read_text(encoding="utf-8"))
            for relative in lock["source_hashes"]:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO_ROOT / relative, destination)
            with (root / "CMakeLists.txt").open("a", encoding="utf-8") as handle:
                handle.write("\n# drift\n")
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--repo-root", root,
                "--inventory-dir", root / "empty-inventory",
                expect_success=False,
            )
            self.assertIn("fallback is stale for CMakeLists.txt", failed.stderr)

    def test_fallback_rejects_each_production_packet_limit_drift(self) -> None:
        """Bind both production size-limit headers to fallback fuzz bounds."""
        relative_headers = (
            "src/lib/execution/isolation/isolated_cpu_invocation_protocol.hpp",
            "src/lib/server/worker/worker_protocol.hpp",
        )
        for relative_header in relative_headers:
            with self.subTest(relative_header=relative_header), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                lock_source = REPO_ROOT / "ci/locks/current-main-profiles-v1.json"
                lock_target = root / "ci/locks/current-main-profiles-v1.json"
                lock_target.parent.mkdir(parents=True)
                shutil.copyfile(lock_source, lock_target)
                lock = json.loads(lock_source.read_text(encoding="utf-8"))
                for relative in lock["source_hashes"]:
                    destination = root / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(REPO_ROOT / relative, destination)
                with (root / relative_header).open("a", encoding="utf-8") as handle:
                    handle.write("\n// packet limit drift\n")
                failed = run_command(
                    "python3", SCRIPTS / "ci_profile_manifest.py",
                    "--repo-root", root,
                    "--inventory-dir", root / "empty-inventory",
                    expect_success=False,
                )
                self.assertIn(f"fallback is stale for {relative_header}", failed.stderr)

    def test_complete_versioned_identity_is_cross_digest_bound(self) -> None:
        """Accept canonical roles only when their matrix sidecar and digest agree."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            digest = self._write_complete_versioned_identity(inventory)
            roles = inventory / "ci_security_roles_v1.tsv"
            completed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
            )
            resolved = json.loads(completed.stdout)
            self.assertFalse(resolved["fallback"])
            self.assertEqual(resolved["matrix_sha256"], digest)
            self.assertEqual(len(resolved["profiles"]), 3)
            fuzz = next(profile for profile in resolved["profiles"] if profile["profile"] == "fuzz-codecs")
            self.assertEqual(
                fuzz["cmake_args"],
                [
                    "-DBUILD_TESTING=OFF",
                    "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                    "-DUSE_ASAN=OFF",
                    "-DUSE_TSAN=OFF",
                ],
            )

            roles.write_text(roles.read_text(encoding="utf-8").replace(digest, "0" * 64), encoding="utf-8")
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("matrix digest does not match", failed.stderr)

    def test_versioned_matrix_rejects_malformed_records_and_isolation(self) -> None:
        """Reject bad fields, IDs, platforms, bounds, references, and isolation."""
        mutations = (
            ("\"roles\":[\"fuzz-artifact\",\"fuzz-codec\"]", "\"roles\":[\"fuzz-artifact\",\"unknown-role\"]", "unknown role"),
            ("-DUSE_ASAN=ON", "-DUSE_ASAN=OFF", "CMake isolation"),
            ("role\tfuzz-codec\t", "role\tfuzz-codec_\t", "invalid identifier"),
            ("\"seed\":1", "\"seed\":2", "requires seed 1"),
            ("\"timeout_seconds\":2}", "\"timeout_seconds\":2,\"unknown\":true}", "fields differ"),
            ("\"platforms\":[\"Darwin\",\"Linux\"]", "\"platforms\":[\"Linux\",\"Windows\"]", "unsupported capability platform"),
        )
        for old, new, diagnostic in mutations:
            with self.subTest(diagnostic=diagnostic), tempfile.TemporaryDirectory() as temporary_text:
                inventory = Path(temporary_text)
                self._write_complete_versioned_identity(inventory)
                matrix = inventory / "build_profile_matrix_v1.tsv"
                original = matrix.read_text(encoding="utf-8")
                self.assertIn(old, original)
                matrix.write_text(original.replace(old, new, 1), encoding="utf-8")
                self._refresh_versioned_digest(inventory)
                failed = run_command(
                    "python3", SCRIPTS / "ci_profile_manifest.py",
                    "--inventory-dir", inventory,
                    expect_success=False,
                )
                self.assertIn(diagnostic, failed.stderr)

    def test_versioned_matrix_rejects_noncanonical_record_order(self) -> None:
        """Reject a byte-valid matrix whose semantic records are reordered."""
        with tempfile.TemporaryDirectory() as temporary_text:
            inventory = Path(temporary_text)
            self._write_complete_versioned_identity(inventory)
            matrix = inventory / "build_profile_matrix_v1.tsv"
            lines = matrix.read_text(encoding="utf-8").splitlines()
            lines[1], lines[2] = lines[2], lines[1]
            matrix.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self._refresh_versioned_digest(inventory)
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("not bytewise sorted", failed.stderr)

    def test_versioned_fuzz_matrix_arguments_reach_real_runner_boundary(self) -> None:
        """Carry the exact three-file matrix CMake args into configure and fuzz."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            inventory = root / "inventory"
            inventory.mkdir()
            self._write_complete_versioned_identity(inventory)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            configure_log = root / "configure.log"
            fuzz_log = root / "fuzz.log"
            uname = binary_dir / "uname"
            uname.write_text(
                "#!/usr/bin/env bash\n"
                "[[ ${1:-} == -s ]] && { echo Linux; exit 0; }\n"
                "[[ ${1:-} == -m ]] && { echo x86_64; exit 0; }\n"
                "exit 1\n",
                encoding="utf-8",
            )
            uname.chmod(0o755)
            cmake = binary_dir / "cmake"
            cmake.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "if [[ ${1:-} == --build && ${4:-} == help ]]; then\n"
                "  echo '... fuzz_codec'; exit 0\n"
                "fi\n"
                "if [[ ${1:-} == --build ]]; then exit 0; fi\n"
                "printf '%q ' \"$@\" > \"$CI_TEST_CONFIGURE_LOG\"; printf '\\n' >> \"$CI_TEST_CONFIGURE_LOG\"\n"
                "build_dir=\n"
                "while (($#)); do [[ $1 == -B ]] && { build_dir=$2; break; }; shift; done\n"
                "mkdir -p \"$build_dir/tests\"\n"
                "printf '%s\\n' '#!/usr/bin/env bash' 'printf '\"'\"'%q '\"'\"' \"$@\" >> \"$CI_TEST_FUZZ_LOG\"; printf '\"'\"'\\n'\"'\"' >> \"$CI_TEST_FUZZ_LOG\"' > \"$build_dir/tests/fuzz_codec\"\n"
                "chmod +x \"$build_dir/tests/fuzz_codec\"\n",
                encoding="utf-8",
            )
            cmake.chmod(0o755)
            artifacts = root / "artifacts"
            run_command(
                "bash", SCRIPTS / "fuzz_smoke.sh",
                environment={
                    "BUILD_DIR": str(root / "build"),
                    "CI_ARTIFACT_DIR": str(artifacts),
                    "CI_INVENTORY_DIR": str(inventory),
                    "CI_JOBS": "1",
                    "CI_TEST_CONFIGURE_LOG": str(configure_log),
                    "CI_TEST_FUZZ_LOG": str(fuzz_log),
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                },
            )
            configured = configure_log.read_text(encoding="utf-8")
            for argument in (
                "-DBUILD_TESTING=OFF",
                "-DPHOTOSPIDER_BUILD_FUZZERS=ON",
                "-DUSE_ASAN=OFF",
                "-DUSE_TSAN=OFF",
            ):
                self.assertIn(argument, configured)
            fuzzed = fuzz_log.read_text(encoding="utf-8")
            for argument in ("-seed=1", "-runs=1000", "-timeout=2", "-max_len=65536"):
                self.assertIn(argument, fuzzed)


class SecurityPlatformContractTest(unittest.TestCase):
    """Exercise Darwin runner locks and fresh vcpkg materialization."""

    @staticmethod
    def _prepare_darwin_fixture(
        root: Path,
    ) -> tuple[Path, dict[str, str], Path, Path, Path, Path]:
        """Create one exact Darwin runner, Git-object, and binary fixture.

        Args:
            root: Empty temporary directory that owns all fixture state.

        Returns:
            The resolved profile, base environment, Git log, vcpkg log,
            runner-temp directory, and deliberately dirty preinstalled source.

        Note:
            The Git shim implements only the read/fetch/checkout/status boundary
            used by ``security_platform_prepare.sh``. It reports the source as
            dirty if a caller inspects worktree diffs, while the exact locked
            commit object remains available for a clean fresh checkout.
        """
        profile = root / "profile.json"
        run_command(
            "python3", SCRIPTS / "ci_profile_manifest.py",
            "--profile", "fuzz-codecs",
            "--output", profile,
        )
        binary_dir = root / "bin"
        binary_dir.mkdir()
        uname = binary_dir / "uname"
        uname.write_text(
            "#!/usr/bin/env bash\n"
            "[[ ${1:-} == -s ]] && { echo Darwin; exit 0; }\n"
            "[[ ${1:-} == -m ]] && { echo arm64; exit 0; }\n"
            "exit 1\n",
            encoding="utf-8",
        )
        uname.chmod(0o755)

        git_log = root / "git.log"
        git = binary_dir / "git"
        git.write_text(
            f"#!{sys.executable}\n"
            "import json\n"
            "import os\n"
            "import sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "with open(os.environ['CI_TEST_GIT_LOG'], 'a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(args, separators=(',', ':')) + '\\n')\n"
            "worktree = None\n"
            "index = 0\n"
            "while index < len(args):\n"
            "    if args[index] == '-C':\n"
            "        worktree = Path(args[index + 1])\n"
            "        index += 2\n"
            "    elif args[index] == '-c':\n"
            "        index += 2\n"
            "    elif args[index].startswith('-'):\n"
            "        index += 1\n"
            "    else:\n"
            "        break\n"
            "command = args[index] if index < len(args) else ''\n"
            "command_args = args[index + 1:]\n"
            "source = Path(os.environ['CI_TEST_VCPKG_SOURCE'])\n"
            "locked = os.environ['CI_TEST_LOCKED_COMMIT']\n"
            "if command == 'init':\n"
            "    target = Path(command_args[-1])\n"
            "    (target / '.git').mkdir(parents=True)\n"
            "elif command == 'cat-file':\n"
            "    raise SystemExit(0 if os.environ.get('CI_TEST_SOURCE_HAS_COMMIT', '1') == '1' else 1)\n"
            "elif command == 'checkout':\n"
            "    toolchain = worktree / 'scripts/buildsystems/vcpkg.cmake'\n"
            "    toolchain.parent.mkdir(parents=True)\n"
            "    toolchain.write_text('# locked fixture\\n', encoding='utf-8')\n"
            "    unsafe = os.environ.get('CI_TEST_UNSAFE_FRESH', '')\n"
            "    if unsafe == 'symlink':\n"
            "        (worktree / 'unsafe-link').symlink_to(source / 'tracked-dirty')\n"
            "    elif unsafe == 'fifo':\n"
            "        os.mkfifo(worktree / 'unsafe-fifo')\n"
            "    elif unsafe == 'residual':\n"
            "        (worktree / 'residual-state').write_text('unexpected\\n', encoding='utf-8')\n"
            "elif command == 'rev-parse':\n"
            "    print(os.environ.get('CI_TEST_FRESH_COMMIT', locked) if worktree != source else locked)\n"
            "elif command == 'status':\n"
            "    if worktree == source:\n"
            "        print(' M tracked-dirty')\n"
            "    elif os.environ.get('CI_TEST_UNSAFE_FRESH') == 'residual':\n"
            "        print('?? residual-state')\n"
            "elif command == 'diff' and worktree == source:\n"
            "    raise SystemExit(1)\n",
            encoding="utf-8",
        )
        git.chmod(0o755)

        vcpkg_source = root / "preinstalled-vcpkg"
        (vcpkg_source / ".git").mkdir(parents=True)
        (vcpkg_source / "tracked-dirty").write_text("dirty runner source\n", encoding="utf-8")
        vcpkg_log = root / "vcpkg.log"
        vcpkg = vcpkg_source / "vcpkg"
        vcpkg.write_text(
            "#!/usr/bin/env bash\n"
            "set -Eeuo pipefail\n"
            "{\n"
            "  printf 'executable=%s\\n' \"$0\"\n"
            "  printf 'root=%s\\n' \"${VCPKG_ROOT:-}\"\n"
            "  printf 'binary=%s\\n' \"${VCPKG_BINARY_SOURCES:-}\"\n"
            "  printf 'asset=%s\\n' \"${X_VCPKG_ASSET_SOURCES:-}\"\n"
            "  for argument in \"$@\"; do printf 'arg=%s\\n' \"$argument\"; done\n"
            "} > \"$CI_TEST_VCPKG_LOG\"\n",
            encoding="utf-8",
        )
        vcpkg.chmod(0o755)
        runner_temp = root / "runner-temp"
        runner_temp.mkdir()
        output = root / "cmake-args.txt"
        locked_commit = "6d9d7df564a1ccdaa994e4ad39ccd4a32360867b"
        environment = {
            "PATH": f"{binary_dir}:{os.environ['PATH']}",
            "CI_PLATFORM_CMAKE_ARGS_FILE": str(output),
            "CI_RUNNER_TEMP": str(runner_temp),
            "CI_TEST_GIT_LOG": str(git_log),
            "CI_TEST_LOCKED_COMMIT": locked_commit,
            "CI_TEST_VCPKG_LOG": str(vcpkg_log),
            "CI_TEST_VCPKG_SOURCE": str(vcpkg_source),
            "ImageOS": "macos15",
            "ImageVersion": "20260727.0256.1",
            "RUNNER_IMAGE_NAME": "macos-15",
            "VCPKG_INSTALLATION_ROOT": str(vcpkg_source),
        }
        return profile, environment, git_log, vcpkg_log, runner_temp, vcpkg_source

    def test_dirty_source_produces_fresh_clean_checkout_and_exact_install(self) -> None:
        """Use a dirty image source only as locked objects, then install fresh."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, git_log, vcpkg_log, runner_temp, source = (
                self._prepare_darwin_fixture(root)
            )
            run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment=environment,
            )
            arguments = (root / "cmake-args.txt").read_text(encoding="utf-8")
            self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-osx", arguments)
            self.assertIn(str(runner_temp), arguments)
            self.assertNotIn(str(source / "scripts/buildsystems/vcpkg.cmake"), arguments)

            install_lines = vcpkg_log.read_text(encoding="utf-8").splitlines()
            executable = next(line.removeprefix("executable=") for line in install_lines if line.startswith("executable="))
            fresh_root = next(line.removeprefix("root=") for line in install_lines if line.startswith("root="))
            self.assertEqual(executable, f"{fresh_root}/vcpkg")
            self.assertTrue(Path(fresh_root).is_relative_to(runner_temp.resolve()))
            self.assertNotEqual(executable, str(source / "vcpkg"))
            self.assertIn("binary=clear", install_lines)
            self.assertIn("asset=clear", install_lines)
            install_arguments = [line.removeprefix("arg=") for line in install_lines if line.startswith("arg=")]
            self.assertEqual(
                install_arguments,
                [
                    "install",
                    "--triplet",
                    "arm64-osx",
                    "--x-install-root",
                    f"{fresh_root}/installed",
                    "--clean-after-build",
                    "openssl",
                    "utf8proc",
                ],
            )
            git_calls = [json.loads(line) for line in git_log.read_text(encoding="utf-8").splitlines()]
            source_identity = str(source.resolve())
            self.assertTrue(any("cat-file" in call and source_identity in call for call in git_calls))
            self.assertFalse(any("diff" in call and source_identity in call for call in git_calls))
            self.assertFalse(any("status" in call and source_identity in call for call in git_calls))

            failed = run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment={
                    **environment,
                    "ImageVersion": "stale",
                    "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / "stale.txt"),
                },
                expect_success=False,
            )
            self.assertIn("differs from protected", failed.stderr)

    def test_fresh_checkout_rejects_commit_link_fifo_and_residual_state(self) -> None:
        """Fail closed for every fresh-checkout identity and filesystem drift."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, _, _, _, _ = self._prepare_darwin_fixture(root)
            cases = (
                ({"CI_TEST_FRESH_COMMIT": "2" * 40}, "differs from protected"),
                ({"CI_TEST_UNSAFE_FRESH": "symlink"}, "contains a link"),
                ({"CI_TEST_UNSAFE_FRESH": "fifo"}, "contains a special entry"),
                ({"CI_TEST_UNSAFE_FRESH": "residual"}, "contains residual or modified state"),
            )
            for index, (override, diagnostic) in enumerate(cases):
                with self.subTest(diagnostic=diagnostic):
                    failed = run_command(
                        "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                        environment={
                            **environment,
                            **override,
                            "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / f"unsafe-{index}.txt"),
                        },
                        expect_success=False,
                    )
                    self.assertIn(diagnostic, failed.stderr)

    def test_missing_preinstalled_object_uses_only_explicit_official_source(self) -> None:
        """Fetch the exact commit from microsoft/vcpkg when local objects lack it."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            profile, environment, git_log, _, _, _ = self._prepare_darwin_fixture(root)
            run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment={**environment, "CI_TEST_SOURCE_HAS_COMMIT": "0"},
            )
            calls = [json.loads(line) for line in git_log.read_text(encoding="utf-8").splitlines()]
            fetch = next(call for call in calls if "fetch" in call)
            self.assertIn("https://github.com/microsoft/vcpkg.git", fetch)
            self.assertIn(environment["CI_TEST_LOCKED_COMMIT"], fetch)

        workflow = (REPO_ROOT / ".github/workflows/ci-integration.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn("CI_RUNNER_TEMP: ${{ runner.temp }}", workflow)
        self.assertNotIn("CI_DARWIN_VCPKG_INSTALLED:", workflow)


class ReusableBuildContractTest(unittest.TestCase):
    """Exercise deterministic packing, exact identity checks, and safe extraction."""

    @staticmethod
    def _load_reusable_module() -> object:
        """Load the protected producer module for deterministic fixture emission."""
        module_path = SCRIPTS / "reusable_build.py"
        specification = importlib.util.spec_from_file_location("reusable_build_contract", module_path)
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load reusable build module")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    @staticmethod
    def _prepare_build(source: Path) -> Path:
        """Materialize one complete fresh CMake/package/generated build fixture."""
        source.mkdir(parents=True, exist_ok=True)
        executable = source / "bin/tool"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        (source / "CMakeCache.txt").write_text(
            "BUILD_TESTING:BOOL=ON\n"
            "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            "CMAKE_PROJECT_NAME:STATIC=photospider\n"
            "PHOTOSPIDER_BUILD_IPC:BOOL=ON\n"
            "USE_ASAN:BOOL=OFF\n"
            "USE_TSAN:BOOL=OFF\n",
            encoding="utf-8",
        )
        module_dir = source / "CMakeFiles/3.31.0"
        module_dir.mkdir(parents=True, exist_ok=True)
        (module_dir / "CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER "/usr/bin/clang++")\n'
            'set(CMAKE_CXX_COMPILER_ID "Clang")\n'
            'set(CMAKE_CXX_COMPILER_VERSION "18.1.8")\n',
            encoding="utf-8",
        )
        (module_dir / "CMakeSystem.cmake").write_text(
            'set(CMAKE_SYSTEM_NAME "Linux")\n'
            'set(CMAKE_SYSTEM_PROCESSOR "x86_64")\n',
            encoding="utf-8",
        )
        (source / "PhotospiderConfig.cmake").write_text(
            "set(Photospider_embedded_FOUND TRUE)\n"
            "set(Photospider_policy_sdk_FOUND ON)\n",
            encoding="utf-8",
        )
        export = source / "CMakeFiles/Export/fixture/PhotospiderTargets.cmake"
        export.parent.mkdir(parents=True, exist_ok=True)
        export.write_text(
            "add_library(Photospider::photospider_kernel STATIC IMPORTED)\n"
            "add_library(Photospider::policy_sdk INTERFACE IMPORTED)\n",
            encoding="utf-8",
        )
        inventory = source / "generated/ci_inventory"
        inventory.mkdir(parents=True, exist_ok=True)
        (inventory / "installable_public_headers.txt").write_text(
            "include/photospider/core/graph.hpp\n", encoding="utf-8"
        )
        (inventory / "registered_gtest_targets-RelWithDebInfo.tsv").write_text(
            "# target\tconfigured executable\n"
            "test_contract\t/tmp/test_contract\n",
            encoding="utf-8",
        )
        (source / ".photospider-ci-build-complete").write_text(
            f"build_dir={source.resolve()}\n"
            f"source_dir={REPO_ROOT.resolve()}\n"
            "profile=default\n"
            "build_testing=ON\n"
            "photospider_build_ipc=ON\n"
            f"candidate_commit={COMMIT_A}\n"
            "created_at=2026-08-25T12:00:00Z\n",
            encoding="utf-8",
        )
        return inventory

    def _create(self, root: Path, suffix: str) -> tuple[Path, Path, dict[str, object]]:
        """Create one test archive and return its paths and parsed manifest."""
        source = root / "source"
        inventory = self._prepare_build(source)
        archive = root / f"ci-build-{suffix}.tar.gz"
        manifest = root / f"ci-build-{suffix}.manifest.json"
        run_command(
            "python3", SCRIPTS / "reusable_build.py",
            "--inventory-dir", inventory,
            "create",
            "--source", source,
            "--archive", archive,
            "--manifest", manifest,
            "--candidate-commit", COMMIT_A,
            "--profile", "default",
            "--image-digest", IMAGE_DIGEST,
            "--workflow-commit", COMMIT_B,
        )
        return archive, manifest, json.loads(manifest.read_text(encoding="utf-8"))

    def test_deterministic_archive_and_exact_safe_extraction(self) -> None:
        """Produce identical archives and reject a wrong expected candidate."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive_a, manifest_a, identity = self._create(root, "a")
            archive_b, _, _ = self._create(root, "b")
            self.assertEqual(hashlib.sha256(archive_a.read_bytes()).digest(), hashlib.sha256(archive_b.read_bytes()).digest())
            destination = root / "restored"
            common = (
                "--archive", archive_a,
                "--manifest", manifest_a,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )
            run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-extract",
                *common,
                "--candidate-commit", COMMIT_A,
                "--destination", destination,
            )
            self.assertEqual((destination / "ci/bin/tool").read_text(encoding="utf-8"), "#!/bin/sh\nexit 0\n")
            failed = run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-only",
                *common,
                "--candidate-commit", "4" * 40,
                expect_success=False,
            )
            self.assertIn("candidate_commit mismatch", failed.stderr)

    def test_traversal_member_is_rejected_after_digest_match(self) -> None:
        """Reject a traversal entry even when a caller recomputes the archive hash."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            archive, manifest, identity = self._create(root, "unsafe")
            with archive.open("wb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                    with tarfile.open(fileobj=compressed, mode="w") as tar:
                        information = tarfile.TarInfo("ci/../escape")
                        information.size = 1
                        tar.addfile(information, io.BytesIO(b"x"))
            identity["artifact"]["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
            identity["artifact"]["size"] = archive.stat().st_size
            manifest.write_text(
                json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
            failed = run_command(
                "python3", SCRIPTS / "reusable_build.py", "verify-only",
                "--archive", archive,
                "--manifest", manifest,
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--matrix-sha256", identity["matrix_sha256"],
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
                expect_success=False,
            )
            self.assertIn("unsafe archive member path", failed.stderr)

    def test_consumer_verifies_two_attestations_before_extraction(self) -> None:
        """Require archive and manifest attestation calls on the generic consumer."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            self._prepare_build(source)
            (source / "result.txt").write_text("verified\n", encoding="utf-8")
            reusable = root / "reusable"
            reusable.mkdir()
            archive = reusable / "ci-build.tar.gz"
            manifest = reusable / "ci-build.manifest.json"
            run_command(
                "python3", SCRIPTS / "reusable_build.py",
                "--inventory-dir", source / "generated/ci_inventory",
                "create",
                "--source", source,
                "--archive", archive,
                "--manifest", manifest,
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )
            identity = json.loads(manifest.read_text(encoding="utf-8"))
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            gh = binary_dir / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_GH_LOG\"; printf '\\n' >> \"$CI_TEST_GH_LOG\"\n"
                "printf '[]\\n'\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            destination = root / "destination"
            run_command(
                "bash", SCRIPTS / "reusable_build_consume.sh",
                environment={
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                    "CI_BUILD_PROFILE": "default",
                    "CI_CANDIDATE_COMMIT": COMMIT_A,
                    "CI_IMAGE_DIGEST": IMAGE_DIGEST,
                    "CI_MATRIX_SHA256": str(identity["matrix_sha256"]),
                    "CI_REUSABLE_DESTINATION": str(destination),
                    "CI_REUSABLE_DIR": str(reusable),
                    "CI_TEST_GH_LOG": str(gh_log),
                    "CI_WORKFLOW_COMMIT": COMMIT_B,
                    "GITHUB_REPOSITORY": "kevin-zf1123/photospider",
                },
            )
            self.assertEqual((destination / "ci/result.txt").read_text(encoding="utf-8"), "verified\n")
            attestations = gh_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(attestations), 2)
            self.assertTrue(all("--deny-self-hosted-runners" in line for line in attestations))

    def test_producer_rejects_cached_empty_and_caller_forged_identity(self) -> None:
        """Reject stale origins, missing measured surfaces, and forged compiler data."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            source = root / "source"
            inventory = self._prepare_build(source)
            common = (
                "python3", SCRIPTS / "reusable_build.py",
                "--inventory-dir", inventory,
                "create",
                "--source", source,
                "--archive", root / "candidate.tar.gz",
                "--manifest", root / "candidate.manifest.json",
                "--candidate-commit", COMMIT_A,
                "--profile", "default",
                "--image-digest", IMAGE_DIGEST,
                "--workflow-commit", COMMIT_B,
            )

            stamp = source / ".photospider-ci-build-complete"
            valid_stamp = stamp.read_text(encoding="utf-8")
            stamp.write_text(valid_stamp.replace(f"build_dir={source.resolve()}", "build_dir=/cached/tree"), encoding="utf-8")
            cached = run_command(*common, expect_success=False)
            self.assertIn("completion stamp build_dir mismatch", cached.stderr)
            stamp.write_text(valid_stamp, encoding="utf-8")

            package_config = source / "PhotospiderConfig.cmake"
            valid_config = package_config.read_text(encoding="utf-8")
            package_config.write_text("# empty package surface\n", encoding="utf-8")
            empty = run_command(*common, expect_success=False)
            self.assertIn("component/target inventory is empty", empty.stderr)
            package_config.write_text(valid_config, encoding="utf-8")

            digest = ProfileReaderContractTest._write_complete_versioned_identity(inventory)
            module = self._load_reusable_module()
            measured = module._measured_identity_value(
                REPO_ROOT,
                source,
                inventory,
                COMMIT_A,
                "default",
                digest,
                False,
            )
            measured["compiler"]["cxx_compiler_id"] = "caller-forged"
            (inventory / "reusable_build_identity_v1.json").write_text(
                json.dumps(measured, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
            forged = run_command(*common, expect_success=False)
            self.assertIn("differs from protected measurement", forged.stderr)

    def test_protected_build_entry_rejects_a_preexisting_output_tree(self) -> None:
        """Execute the maintained freshness helper against cached and empty paths."""
        source = (SCRIPTS / "build_integrity.sh").read_text(encoding="utf-8")
        match = re.search(
            r"(?ms)^prepare_fresh_producer_build\(\) \{.*?^\}\n\nprepare_fresh_producer_build$",
            source,
        )
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory() as temporary_text:
            repository = Path(temporary_text) / "repository"
            build = repository / "build/ci"
            build.mkdir(parents=True)
            harness = Path(temporary_text) / "fresh-build-contract.sh"
            harness.write_text(
                "#!/usr/bin/env bash\nset -Eeuo pipefail\n" + match.group(0) + "\n",
                encoding="utf-8",
            )
            harness.chmod(0o755)
            cached = run_command(
                "bash", harness,
                environment={"REPO_ROOT": str(repository), "BUILD_DIR": str(build)},
                expect_success=False,
            )
            self.assertIn("refuses a cached or residual build directory", cached.stderr)
            build.rmdir()
            run_command(
                "bash", harness,
                environment={"REPO_ROOT": str(repository), "BUILD_DIR": str(build)},
            )
            self.assertTrue(build.is_dir())
            self.assertFalse(build.is_symlink())


class RulesetContractTest(unittest.TestCase):
    """Exercise exact stable checks and coherent default-branch policy."""

    def test_valid_fixture_passes_and_incoherent_fixture_fails(self) -> None:
        """Accept only the exact GitHub Actions-bound stable check pair."""
        base = {
            "conditions": {"ref_name": {"exclude": [], "include": ["~DEFAULT_BRANCH"]}},
            "enforcement": "active",
            "id": 1,
            "name": "main protect",
            "rules": [
                {"type": "deletion"},
                {"type": "non_fast_forward"},
                {"type": "required_linear_history"},
                {
                    "parameters": {
                        "allowed_merge_methods": ["rebase", "squash"],
                        "required_review_thread_resolution": True,
                    },
                    "type": "pull_request",
                },
                {
                    "parameters": {
                        "required_status_checks": [
                            {"context": "healthcheck", "integration_id": 15368},
                            {"context": "integration", "integration_id": 15368},
                        ],
                        "strict_required_status_checks_policy": True,
                    },
                    "type": "required_status_checks",
                },
            ],
            "target": "branch",
        }
        with tempfile.TemporaryDirectory() as temporary_text:
            valid = Path(temporary_text) / "valid.json"
            invalid = Path(temporary_text) / "invalid.json"
            valid.write_text(json.dumps([base]), encoding="utf-8")
            broken = json.loads(json.dumps(base))
            broken["rules"][3]["parameters"]["required_review_thread_resolution"] = False
            broken["rules"][3]["parameters"]["allowed_merge_methods"] = ["merge"]
            invalid.write_text(json.dumps([broken]), encoding="utf-8")
            run_command(
                "python3", SCRIPTS / "ruleset_readback.py",
                "--input", valid,
            )
            failed = run_command(
                "python3", SCRIPTS / "ruleset_readback.py",
                "--input", invalid,
                expect_success=False,
            )
            self.assertIn("required conversation resolution is disabled", failed.stderr)

    def test_live_readback_aggregates_later_ruleset_pages(self) -> None:
        """Find a second default-branch ruleset after the first 30 summaries."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            gh_log = root / "gh.log"
            gh = binary_dir / "gh"
            gh.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "args = sys.argv[1:]\n"
                "with open(os.environ['CI_TEST_GH_LOG'], 'a', encoding='utf-8') as handle:\n"
                "    handle.write(json.dumps(args, separators=(',', ':')) + '\\n')\n"
                "endpoint = args[-1]\n"
                "if endpoint.endswith('rulesets?includes_parents=false'):\n"
                "    first = [{'id': identifier} for identifier in range(1, 31)]\n"
                "    second = [{'id': 31}]\n"
                "    print(json.dumps([first, second] if '--paginate' in args and '--slurp' in args else first))\n"
                "else:\n"
                "    identifier = int(endpoint.rsplit('/', 1)[1])\n"
                "    active = identifier in (1, 31)\n"
                "    print(json.dumps({\n"
                "        'conditions': {'ref_name': {'exclude': [], 'include': ['~DEFAULT_BRANCH'] if active else []}},\n"
                "        'enforcement': 'active' if active else 'disabled',\n"
                "        'id': identifier,\n"
                "        'name': f'ruleset-{identifier}',\n"
                "        'rules': [],\n"
                "        'target': 'branch',\n"
                "    }))\n",
                encoding="utf-8",
            )
            gh.chmod(0o755)
            failed = run_command(
                "python3", SCRIPTS / "ruleset_readback.py",
                "--repository", "kevin-zf1123/photospider",
                environment={
                    "CI_TEST_GH_LOG": str(gh_log),
                    "PATH": f"{binary_dir}:{os.environ['PATH']}",
                },
                expect_success=False,
            )
            self.assertIn("expected exactly one active default-branch ruleset, found 2", failed.stderr)
            calls = [json.loads(line) for line in gh_log.read_text(encoding="utf-8").splitlines()]
            list_call = calls[0]
            self.assertIn("--paginate", list_call)
            self.assertIn("--slurp", list_call)
            self.assertIn(
                "repos/kevin-zf1123/photospider/rulesets?includes_parents=false",
                list_call,
            )
            self.assertTrue(any(call[-1].endswith("/rulesets/31") for call in calls))


class LockSurfaceContractTest(unittest.TestCase):
    """Exercise the complete active protected lock/workflow verifier."""

    @staticmethod
    def _load_lock_module() -> object:
        """Load the protected lock verifier for isolated workflow fixtures."""
        module_path = SCRIPTS / "ci_lock_verify.py"
        specification = importlib.util.spec_from_file_location(
            "ci_lock_verify_contract", module_path
        )
        if specification is None or specification.loader is None:
            raise AssertionError("cannot load CI lock verifier")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        return module

    def test_unpinned_yaml_workflow_fails_the_same_lock_parser(self) -> None:
        """Reject an unpinned action in the formerly omitted YAML suffix."""
        module = self._load_lock_module()
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
            workflow_root = root / ".github/workflows"
            workflow_root.mkdir(parents=True)
            (workflow_root / "unpinned.yaml").write_text(
                "jobs:\n"
                "  test:\n"
                "    runs-on: ubuntu-24.04\n"
                "    steps:\n"
                "      - uses: actions/checkout@v4 # v4\n",
                encoding="utf-8",
            )
            locked_commit = "1" * 40
            with self.assertRaises(module.ContractError) as raised:
                module._verify_workflows(
                    root, {"actions/checkout": ("v4", locked_commit)}
                )
            self.assertIn(f"uses v4, expected {locked_commit}", str(raised.exception))

    def test_workflow_inventory_rejects_links_specials_and_unknown_entries(self) -> None:
        """Reject every noncanonical directory entry before YAML parsing."""
        module = self._load_lock_module()
        cases = ("symlink", "fifo", "directory", "unknown-extension")
        for unsafe_kind in cases:
            with self.subTest(unsafe_kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary_text:
                root = Path(temporary_text)
                workflow_root = root / ".github/workflows"
                workflow_root.mkdir(parents=True)
                if unsafe_kind == "symlink":
                    outside = root / "outside.yml"
                    outside.write_text("jobs: {}\n", encoding="utf-8")
                    (workflow_root / "unsafe.yml").symlink_to(outside)
                elif unsafe_kind == "fifo":
                    os.mkfifo(workflow_root / "unsafe.yaml")
                elif unsafe_kind == "directory":
                    (workflow_root / "nested.yml").mkdir()
                else:
                    (workflow_root / "README.md").write_text("unexpected\n", encoding="utf-8")
                with self.assertRaises(module.ContractError):
                    module._workflow_files(root)

    def test_repository_lock_surface_is_exact(self) -> None:
        """Reject no active floating actions, runners, images, or install inputs."""
        run_command("python3", SCRIPTS / "ci_lock_verify.py")


if __name__ == "__main__":
    unittest.main(verbosity=2)
