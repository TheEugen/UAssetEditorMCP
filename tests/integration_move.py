"""Move a Blueprint with inherited and soft references across separate Unreal processes."""
import argparse
import hashlib
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    client = Client(args.project, args.engine)
    root = "/Game/__UAssetAgentTests/Move_" + uuid.uuid4().hex
    parent, child, label, dest = (root + "/" + name for name in ("BP_Base", "BP_Child", "Label", "Moved/BP_Renamed"))

    def run(action, expect=True, **kwargs):
        result = client.execute(dict(action=action, **kwargs))
        assert result["ok"] == expect, result
        return result

    def file(asset):
        return (client.project.parent / "Content" / asset.removeprefix("/Game/")).with_suffix(".uasset")

    def digest(path):
        return hashlib.sha1(path.read_bytes()).hexdigest()

    run("create", asset=parent, operations=[], dry_run=False)
    old_class = parent + ".BP_Base_C"
    run("create", asset=child, parent_class=old_class, operations=[], dry_run=False)
    run("asset_create", asset=label, **{"class": "/Script/Engine.PrimaryAssetLabel"},
        operations=[dict(op="array_add", property="ExplicitBlueprints", value=old_class)], dry_run=False)
    for source, target, leave in [(parent, dest, False), (dest, root + "/Final/BP_Final", True)]:
        move = {"from": source, "to": target, "leave_redirector": leave}
        before = {name: digest(file(name)) for name in (source, child, label)}
        preview = run("move_asset", **move)
        assert not preview["saved"] and preview["revisions"] == before and not file(target).exists()
        assert set(preview["referencers"]) == {child, label} and len(preview["compiles"]) >= 2
        assert all(digest(file(name)) == rev for name, rev in before.items())
        stale = dict(before)
        stale[child] = "0" * 40
        assert not run("move_asset", expect=False, **move, expected_revisions=stale, dry_run=False)["saved"]
        saved = run("move_asset", **move, expected_revisions=before, dry_run=False)
        assert saved["saved"] and file(source).exists() == leave
        assert {name: digest(Path(path)) for name, path in saved["backups"].items()} == before
        assert set(run("references", asset=target)["references"]) >= {child, label}
        assert run("compile", path=child)["ok"]
        properties = run("asset_inspect", asset=label)["properties"]
        assert target in properties["ExplicitBlueprints"]["value"] and source not in properties["ExplicitBlueprints"]["value"]
        if leave:
            results = run("redirectors", path=root)["redirectors"]
            assert any(item["resolved_destination"].startswith(target + ".") for item in results), results
    print("Verified preview, revisions, backup hashes, inheritance, soft references and both redirector modes.")


if __name__ == "__main__":
    main()
