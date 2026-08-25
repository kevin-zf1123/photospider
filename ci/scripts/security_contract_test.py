#!/usr/bin/env python3
"""Durable regressions for protected CI identity and fail-closed readers."""

from __future__ import annotations

import gzip
import hashlib
import importlib.util
import io
import json
import os
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
COMMIT_A = "1" * 40
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
            verified = module.verify(REPO_ROOT, "Linux")
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
            module.verify(REPO_ROOT, "Linux")


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
        matrix = inventory / "build_profile_matrix_v1.tsv"
        matrix.write_text(
            "schema\tphotospider-build-profile-matrix-v1\n"
            "profile\tfuzz-codecs\n"
            "profile\tsanitizer-asan\n"
            "profile\tsanitizer-tsan\n",
            encoding="utf-8",
        )
        digest = hashlib.sha256(matrix.read_bytes()).hexdigest()
        (inventory / "build_profile_matrix_v1.tsv.sha256").write_text(
            f"{digest}  build_profile_matrix_v1.tsv\n",
            encoding="utf-8",
        )
        records = [
            "fuzz\tfuzz-codecs\t[\"Darwin\",\"Linux\"]\t[\"openssl\",\"utf8proc\"]\tfuzz_codec\t1\t1000\t2\t20\t65536\t65536\tps::codec::kLimit\tnone\tfuzz-smoke",
            "sanitizer\tsanitizer-asan\t[\"Darwin\",\"Linux\"]\t[\"-DUSE_ASAN=ON\",\"-DUSE_TSAN=OFF\"]\t[\"openssl\",\"utf8proc\"]\t[\"test_contract\"]\t[\"security\"]\tsanitizer-smoke",
            "sanitizer\tsanitizer-tsan\t[\"Darwin\",\"Linux\"]\t[\"-DUSE_ASAN=OFF\",\"-DUSE_TSAN=ON\"]\t[\"openssl\",\"utf8proc\"]\t[\"test_contract\"]\t[\"security\"]\tsanitizer-smoke",
        ]
        (inventory / "ci_security_roles_v1.tsv").write_text(
            "schema\tphotospider-ci-security-roles-v1\n"
            f"matrix_sha256\t{digest}\n"
            + "\n".join(sorted(records)) + "\n",
            encoding="utf-8",
        )
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

            roles.write_text(roles.read_text(encoding="utf-8").replace(digest, "0" * 64), encoding="utf-8")
            failed = run_command(
                "python3", SCRIPTS / "ci_profile_manifest.py",
                "--inventory-dir", inventory,
                expect_success=False,
            )
            self.assertIn("matrix digest does not match", failed.stderr)


class SecurityPlatformContractTest(unittest.TestCase):
    """Exercise Darwin runner/vcpkg locks and platform eligibility."""

    def test_darwin_exact_runner_and_registry_prepare_toolchain(self) -> None:
        """Accept exact mocked hosted identities and reject image-version drift."""
        with tempfile.TemporaryDirectory() as temporary_text:
            root = Path(temporary_text)
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
                "[[ ${1:-} == -m ]] && { echo x86_64; exit 0; }\n"
                "exit 1\n",
                encoding="utf-8",
            )
            uname.chmod(0o755)
            git = binary_dir / "git"
            git.write_text(
                "#!/usr/bin/env bash\n"
                "echo 6d9d7df564a1ccdaa994e4ad39ccd4a32360867b\n",
                encoding="utf-8",
            )
            git.chmod(0o755)
            vcpkg_root = root / "vcpkg"
            (vcpkg_root / ".git").mkdir(parents=True)
            (vcpkg_root / "scripts/buildsystems").mkdir(parents=True)
            (vcpkg_root / "scripts/buildsystems/vcpkg.cmake").write_text("# fixture\n", encoding="utf-8")
            vcpkg_log = root / "vcpkg.log"
            vcpkg = vcpkg_root / "vcpkg"
            vcpkg.write_text(
                "#!/usr/bin/env bash\n"
                "set -Eeuo pipefail\n"
                "printf 'binary=%s asset=%s ' \"${VCPKG_BINARY_SOURCES:-}\" \"${X_VCPKG_ASSET_SOURCES:-}\" > \"$CI_TEST_VCPKG_LOG\"\n"
                "printf '%q ' \"$@\" >> \"$CI_TEST_VCPKG_LOG\"\n",
                encoding="utf-8",
            )
            vcpkg.chmod(0o755)
            output = root / "cmake-args.txt"
            environment = {
                "PATH": f"{binary_dir}:{os.environ['PATH']}",
                "CI_DARWIN_VCPKG_INSTALLED": str(root / "installed"),
                "CI_PLATFORM_CMAKE_ARGS_FILE": str(output),
                "CI_TEST_VCPKG_LOG": str(vcpkg_log),
                "ImageOS": "macos15",
                "ImageVersion": "20260727.0377.1",
                "RUNNER_IMAGE_NAME": "macos-15",
                "VCPKG_INSTALLATION_ROOT": str(vcpkg_root),
            }
            run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment=environment,
            )
            arguments = output.read_text(encoding="utf-8")
            self.assertIn("-DCMAKE_TOOLCHAIN_FILE=", arguments)
            self.assertIn("-DVCPKG_TARGET_TRIPLET=x64-osx", arguments)
            install = vcpkg_log.read_text(encoding="utf-8")
            self.assertIn("binary=clear asset=clear", install)
            self.assertIn("openssl", install)
            self.assertIn("utf8proc", install)

            failed = run_command(
                "bash", SCRIPTS / "security_platform_prepare.sh", profile,
                environment={**environment, "ImageVersion": "stale", "CI_PLATFORM_CMAKE_ARGS_FILE": str(root / "stale.txt")},
                expect_success=False,
            )
            self.assertIn("differs from protected", failed.stderr)


class ReusableBuildContractTest(unittest.TestCase):
    """Exercise deterministic packing, exact identity checks, and safe extraction."""

    def _create(self, root: Path, suffix: str) -> tuple[Path, Path, dict[str, object]]:
        """Create one test archive and return its paths and parsed manifest."""
        source = root / "source"
        source.mkdir(exist_ok=True)
        executable = source / "bin/tool"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        executable.chmod(0o755)
        archive = root / f"ci-build-{suffix}.tar.gz"
        manifest = root / f"ci-build-{suffix}.manifest.json"
        run_command(
            "python3", SCRIPTS / "reusable_build.py", "create",
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
            source.mkdir()
            (source / "result.txt").write_text("verified\n", encoding="utf-8")
            reusable = root / "reusable"
            reusable.mkdir()
            archive = reusable / "ci-build.tar.gz"
            manifest = reusable / "ci-build.manifest.json"
            run_command(
                "python3", SCRIPTS / "reusable_build.py", "create",
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


class LockSurfaceContractTest(unittest.TestCase):
    """Exercise the complete active protected lock/workflow verifier."""

    def test_repository_lock_surface_is_exact(self) -> None:
        """Reject no active floating actions, runners, images, or install inputs."""
        run_command("python3", SCRIPTS / "ci_lock_verify.py")


if __name__ == "__main__":
    unittest.main(verbosity=2)
