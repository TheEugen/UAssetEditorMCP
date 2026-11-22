from contextlib import ExitStack
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from publish_release import publish, release_main, TAG_PREFIX, ZIP_NAME
from package_release import release_workspace


@unittest.skipUnless(os.name == "nt", "requires Windows file sharing semantics")
class WorkspaceTests(unittest.TestCase):
    def test_held_log_does_not_block_validated_zip_or_hide_smoke_failure(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as root, ExitStack() as held, \
                    patch("package_release.time.sleep"), patch("sys.stderr", new_callable=io.StringIO) as log:
                def build():
                    with release_workspace(root, "src-") as source:
                        with release_workspace(source, "verify-") as project:
                            held.enter_context((project / "process.log").open("wb"))
                            if fail:
                                raise ValueError("native smoke failed")
                            (source / ZIP_NAME).write_bytes(b"validated")
                        (source / ZIP_NAME).replace(Path(root) / ZIP_NAME)

                if fail:
                    with self.assertRaisesRegex(ValueError, "native smoke failed"):
                        build()
                    self.assertFalse((Path(root) / ZIP_NAME).exists())
                else:
                    build()
                    self.assertEqual((Path(root) / ZIP_NAME).read_bytes(), b"validated")
                self.assertIn("Cleanup deferred", log.getvalue())


class PublicationTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.archive = Path(temp.name) / ZIP_NAME
        self.archive.write_bytes(b"validated archive")
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.sha = "a" * 40
        self.tag = TAG_PREFIX + self.sha
        self.existing = None
        self.latest = None
        self.relation = "ahead"
        self.asset = {"id": 15, "state": "uploaded", "name": ZIP_NAME, "size": self.archive.stat().st_size,
                      "digest": "sha256:" + self.digest}
        self.api = Mock()
        self.api.release.side_effect = lambda tag: self.existing
        self.api.api.side_effect = self.endpoint
        self.mutations = []

    def endpoint(self, path, method="GET", data=None, **options):
        if method != "GET":
            self.mutations.append((method, path, data))
        if path == "releases/latest":
            return self.latest
        if path.startswith("compare/"):
            return {"status": self.relation}
        if path == "releases":
            self.existing = dict(data, id=1, assets=[])
            return self.existing
        if options.get("upload"):
            self.existing["assets"] = [self.asset]
            return self.asset
        if path == "releases/1" and method == "PATCH":
            self.existing.update(data)
            if data["make_latest"] == "true":
                self.latest = self.existing
            return self.existing
        if method != "DELETE":
            self.fail((path, method))

    def release(self):
        publish(self.api, self.archive, self.sha)

    def test_publication_then_rerun_preserves_original_asset(self):
        self.release()
        self.assertTrue(self.mutations[0][2]["draft"])
        self.assertEqual(self.mutations[-1][2]["make_latest"], "true")
        self.assertIn(self.sha, self.latest["body"])
        self.mutations.clear()
        self.archive.write_bytes(b"a later rebuild of the same commit")
        self.release()
        self.assertEqual(self.mutations, [])
        self.api.verify_tag.assert_called_with(self.tag, self.sha, allow_missing=False)

    def test_late_ancestor_is_published_without_changing_latest(self):
        self.latest = {"id": 2, "tag_name": TAG_PREFIX + "b" * 40}
        self.relation = "behind"
        self.release()
        self.assertEqual(self.mutations[-1][2]["make_latest"], "false")
        self.assertEqual(self.latest["id"], 2)

    def test_interrupted_draft_is_repaired_before_publication(self):
        self.existing = {"id": 1, "tag_name": self.tag, "target_commitish": self.sha, "draft": True,
                         "assets": [dict(self.asset, state="starter", digest=None)]}
        self.release()
        self.assertEqual([m[0] for m in self.mutations], ["DELETE", "POST", "PATCH"])

    def test_digest_failure_leaves_draft(self):
        self.asset["digest"] = "sha256:wrong"
        with self.assertRaisesRegex(ValueError, "GitHub did not confirm"):
            self.release()
        self.assertTrue(self.existing["draft"])
        self.assertIsNone(self.latest)

    def test_unpushed_checkout_stops_before_build_or_publication(self):
        with patch("publish_release.engine_build_id"), patch("publish_release.Releases") as client, \
                patch("publish_release.subprocess.check_output", return_value=self.sha), \
                patch("publish_release.subprocess.run") as build:
            client.return_value.api.return_value = {"sha": "b" * 40}
            with self.assertRaisesRegex(ValueError, "Local HEAD differs"):
                release_main(Path("engine"))
            build.assert_not_called()
            client.return_value.release.assert_not_called()

    def test_manual_release_builds_committed_files_and_only_publishes_on_success(self):
        root = self.archive.parent
        def git(*args):
            return subprocess.check_output(["git", *args], cwd=root, text=True).strip()

        git("init", "-q")
        fixture = root / "fixture.txt"
        fixture.write_text("committed")
        git("add", "fixture.txt")
        git("-c", "user.name=Test", "-c", "user.email=test@example.org", "-c", "commit.gpgsign=false",
            "commit", "-qm", "fixture")
        sha = git("rev-parse", "HEAD")
        fixture.write_text("local edit")
        real_run = subprocess.run
        fail_package = False

        def run(command, **kwargs):
            if command[0] == "git":
                return real_run(command, **kwargs)
            self.assertEqual((kwargs["cwd"] / "fixture.txt").read_text(), "committed")
            if "tools/package_release.py" in command:
                if fail_package:
                    raise subprocess.CalledProcessError(1, command)
                (command[-1] / ZIP_NAME).write_bytes(b"validated")

        with patch("publish_release.ROOT", root), patch("publish_release.engine_build_id"), \
                patch("publish_release.Releases", return_value=self.api), patch("publish_release.publish") as upload, \
                patch("publish_release.subprocess.run", side_effect=run):
            self.api.api.side_effect = None
            self.api.api.return_value = {"sha": sha}
            release_main(root / "engine")
            archive = root / "dist" / sha / ZIP_NAME
            self.assertEqual(archive.read_bytes(), b"validated")
            upload.assert_called_once_with(self.api, archive, sha)
            upload.reset_mock()
            fail_package = True
            with self.assertRaises(subprocess.CalledProcessError):
                release_main(root / "engine")
            upload.assert_not_called()
