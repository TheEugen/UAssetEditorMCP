import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from publish_release import publish, TAG_PREFIX, ZIP_NAME


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

    def release(self, run=5):
        publish(self.api, self.archive, self.sha, run, self.digest)

    def test_publication_then_rerun_preserves_original_asset(self):
        self.release()
        self.assertTrue(self.mutations[0][2]["draft"])
        self.assertEqual(self.mutations[-1][2]["make_latest"], "true")
        self.assertIn(self.sha, self.latest["body"])
        self.mutations.clear()
        self.archive.write_bytes(b"a later rebuild of the same commit")
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.release()
        self.assertEqual(self.mutations, [])
        self.api.verify_tag.assert_called_with(self.tag, self.sha, allow_missing=False)

    def test_late_ancestor_is_published_without_changing_latest(self):
        self.latest = {"id": 2, "tag_name": TAG_PREFIX + "b" * 40, "body": "<!-- win64-main-run: 6 -->"}
        self.relation = "behind"
        self.release()
        self.assertEqual(self.mutations[-1][2]["make_latest"], "false")
        self.assertEqual(self.latest["id"], 2)

    def test_interrupted_draft_is_repaired_before_publication(self):
        self.existing = {"id": 1, "tag_name": self.tag, "target_commitish": self.sha, "draft": True,
                         "assets": [dict(self.asset, state="starter", digest=None)]}
        self.release()
        self.assertEqual([m[0] for m in self.mutations], ["DELETE", "POST", "PATCH"])

    def test_digest_failures_never_publish(self):
        self.asset["digest"] = "sha256:wrong"
        with self.assertRaisesRegex(ValueError, "GitHub did not confirm"):
            self.release()
        self.assertTrue(self.existing["draft"])
        self.assertIsNone(self.latest)
        self.mutations.clear()
        self.digest = "corrupt handoff"
        with self.assertRaisesRegex(ValueError, "differs from"):
            self.release()
        self.assertEqual(self.mutations, [])
