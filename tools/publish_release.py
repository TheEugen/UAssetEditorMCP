"""Build, validate and publish the pushed main commit from a Windows release PC."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import zipfile

from package_release import ROOT, ZIP_NAME, engine_build_id, release_workspace
sys.path.insert(0, str(ROOT))
from uasset_agent import project_lock

REPO = "TheEugen/UAssetEditorMCP"
TAG_PREFIX = "ue5.4.4-win64-"


class Releases:
    def __init__(self):
        self.token = subprocess.check_output(["gh", "auth", "token", "--hostname", "github.com"], text=True).strip()

    def api(self, path, method="GET", data=None, *, missing=False, upload=False):
        host = "uploads.github.com" if upload else "api.github.com"
        url = f"https://{host}/repos/{REPO}/{path}"
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


def publish(api, archive, sha):
    if archive.name != ZIP_NAME or not re.fullmatch("[0-9a-f]{40}", sha):
        raise ValueError("Invalid release input")
    payload = archive.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()

    tag = TAG_PREFIX + sha
    release = api.release(tag)
    api.verify_tag(tag, sha, allow_missing=not release or release["draft"])
    latest = api.api("releases/latest", missing=True)
    promote = True
    if latest and latest["tag_name"].startswith(TAG_PREFIX) and latest["tag_name"] != tag:
        latest_sha = latest["tag_name"][len(TAG_PREFIX):]
        if not re.fullmatch("[0-9a-f]{40}", latest_sha):
            raise ValueError("Unrecognized latest Win64 tag")
        relation = api.api(f"compare/{latest_sha}...{sha}?per_page=1")["status"]
        # After a history rewrite, only the current main tip may become Latest.
        promote = relation == "ahead" or (relation == "diverged" and api.api("commits/main")["sha"] == sha)
    if release and not release["draft"]:
        # Rebuilding a DLL need not produce identical bytes. Preserve the
        # original validated public asset, including on interrupted-job retries.
        asset = next((a for a in release["assets"] if a["name"] == ZIP_NAME), None)
        if not asset or asset["state"] != "uploaded" or not asset.get("digest") or asset["digest"] not in release["body"]:
            raise ValueError("Published release has an incomplete or unexpected asset")
        if promote and (not latest or latest["id"] != release["id"]):
            api.api(f"releases/{release['id']}", "PATCH", {"make_latest": "true"})
        print(f"Verified existing release {tag}")
        return

    body = (f"Win64 editor plugin and local MCP server for Epic Launcher UE 5.4.4.\n\n"
            f"Commit: {sha}\n\nZIP checksum: sha256:{digest}")
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


def release_main(engine):
    engine_build_id(engine, local_smoke=False)
    api = Releases()
    sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if sha != api.api("commits/main")["sha"]:
        raise ValueError("Local HEAD differs from GitHub main. Update your release checkout and retry.")
    output = ROOT / "dist" / sha
    output.mkdir(parents=True, exist_ok=True)
    print(f"Building {REPO} at {sha}", flush=True)
    # An archive of the commit excludes local edits and ignored build products.
    with release_workspace(ROOT / "dist", "src-") as work:
        source = work / "source"
        snapshot = work / "source.zip"
        subprocess.run(["git", "archive", "--format=zip", "-o", str(snapshot), sha], cwd=ROOT, check=True)
        with zipfile.ZipFile(snapshot) as tree:
            tree.extractall(source)
        python = engine / "Engine/Binaries/ThirdParty/Python3/Win64/python.exe"
        subprocess.run([python, "-B", "-X", "utf8", "-m", "unittest", "discover", "-s", "tests",
                        "-p", "test_*.py"], cwd=source, check=True)
        subprocess.run([python, "-B", "-X", "utf8", "tools/package_release.py", "--engine", engine,
                        "--output", work], cwd=source, check=True)
        os.replace(work / ZIP_NAME, output / ZIP_NAME)
    publish(api, output / ZIP_NAME, sha)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path, help="Epic Launcher UE 5.4.4 installation")
    args = parser.parse_args()
    # All release checkouts used by this Windows account share one publisher.
    lock = Path(os.environ["LOCALAPPDATA"]) / "UAssetEditorMCP" / "release.lock"
    with project_lock(lock):
        release_main(args.engine.resolve(strict=True))
