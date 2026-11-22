"""Publish a verified Actions artifact. Run inside the serialized publish job."""
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from package_release import ZIP_NAME

TAG_PREFIX = "ue5.4.4-win64-"
ORDER = re.compile(r"^<!-- win64-main-run: (\d+) -->$", re.M)


class Releases:
    def __init__(self):
        self.repo = os.environ["GITHUB_REPOSITORY"]
        self.token = os.environ["GH_TOKEN"]

    def api(self, path, method="GET", data=None, *, missing=False, upload=False):
        host = "uploads.github.com" if upload else "api.github.com"
        url = f"https://{host}/repos/{self.repo}/{path}"
        body = data if upload or data is None else json.dumps(data).encode()
        request = Request(url, data=body, method=method, headers={
            "Authorization": f"Bearer {self.token}", "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/zip" if upload else "application/json"})
        try:
            with urlopen(request, timeout=180) as response:
                raw = response.read()
                return json.loads(raw) if raw else None
        except HTTPError as error:
            if missing and error.code == 404:
                return None
            raise RuntimeError(f"{method} {path}: HTTP {error.code}: {error.read().decode()}") from error

    def release(self, tag):
        release = self.api("releases/tags/" + tag, missing=True)
        if release:
            return release
        # On GitHub a draft can exist before its tag is visible.
        page = 1
        while True:
            releases = self.api(f"releases?per_page=100&page={page}")
            for candidate in releases:
                if candidate["tag_name"] == tag:
                    return candidate
            if len(releases) < 100:
                return None
            page += 1

    def verify_tag(self, tag, sha, *, allow_missing):
        ref = self.api("git/ref/tags/" + tag, missing=allow_missing)
        if ref is None:
            return
        obj = ref["object"]
        while obj["type"] == "tag":
            obj = self.api("git/tags/" + obj["sha"])["object"]
        if obj["type"] != "commit" or obj["sha"] != sha:
            raise ValueError("The release tag already points at another commit")


def release_order(release):
    if not release or not release["tag_name"].startswith(TAG_PREFIX):
        return 0
    match = ORDER.search((release["body"] or "").replace("\r\n", "\n"))
    if not match:
        raise ValueError("Existing Win64 release has no workflow ordering marker")
    return int(match[1])


def publish(api, archive, sha, run_number, expected_digest):
    if archive.name != ZIP_NAME or not re.fullmatch("[0-9a-f]{40}", sha) or run_number < 1:
        raise ValueError("Invalid release input")
    payload = archive.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != expected_digest:
        raise ValueError("Downloaded artifact differs from the validated build")

    tag = TAG_PREFIX + sha
    release = api.release(tag)
    api.verify_tag(tag, sha, allow_missing=not release or release["draft"])
    latest = api.api("releases/latest", missing=True)
    promote = run_number >= release_order(latest)
    if latest and latest["tag_name"].startswith(TAG_PREFIX) and latest["tag_name"] != tag:
        latest_sha = latest["tag_name"][len(TAG_PREFIX):]
        if not re.fullmatch("[0-9a-f]{40}", latest_sha):
            raise ValueError("Unrecognized latest Win64 tag")
        relation = api.api(f"compare/{latest_sha}...{sha}?per_page=1")["status"]
        if relation in ("ahead", "behind"):
            promote = relation == "ahead"
    if release and not release["draft"]:
        # Rebuilding a DLL need not produce identical bytes. Preserve the
        # original validated public asset, including on interrupted-job retries.
        asset = next((a for a in release["assets"] if a["name"] == ZIP_NAME), None)
        if not asset or asset["state"] != "uploaded" or not asset.get("digest") or asset["digest"] not in release["body"]:
            raise ValueError("Published release has an incomplete or unexpected asset")
        previous_order = release_order(release)
        if promote and (not latest or latest["id"] != release["id"] or run_number > previous_order):
            body = ORDER.sub(f"<!-- win64-main-run: {run_number} -->", release["body"].replace("\r\n", "\n"))
            api.api(f"releases/{release['id']}", "PATCH", {"make_latest": "true", "body": body})
        print(f"Verified existing release {tag}")
        return

    body = (f"Win64 editor plugin and local MCP server for Epic Launcher UE 5.4.4.\n\n"
            f"Commit: {sha}\n\nZIP checksum: sha256:{digest}\n\n<!-- win64-main-run: {run_number} -->")
    if not release:
        release = api.api("releases", "POST", {"tag_name": tag, "target_commitish": sha, "draft": True,
                          "name": f"UE 5.4.4 Win64 · {sha[:12]}", "body": body})
    if release["target_commitish"] != sha:
        raise ValueError("Draft target differs from the pushed SHA")
    rid = release["id"]
    asset = next((a for a in release["assets"] if a["name"] == ZIP_NAME), None)
    if asset and (asset["state"] != "uploaded" or asset.get("digest") != "sha256:" + digest):
        api.api(f"releases/assets/{asset['id']}", "DELETE")
        asset = None
    if not asset:
        asset = api.api(f"releases/{rid}/assets?" + urlencode({"name": ZIP_NAME}), "POST", payload, upload=True)
    if asset["state"] != "uploaded" or asset["size"] != len(payload) or asset.get("digest") != "sha256:" + digest:
        raise ValueError("GitHub did not confirm the complete validated ZIP")
    api.api(f"releases/{rid}", "PATCH", {"draft": False, "body": body, "make_latest": str(promote).lower()})
    print(f"Published {tag} (Latest: {promote})")


if __name__ == "__main__":
    publish(Releases(), Path(sys.argv[1]), os.environ["GITHUB_SHA"], int(os.environ["GITHUB_RUN_NUMBER"]),
            os.environ["EXPECTED_SHA256"])
