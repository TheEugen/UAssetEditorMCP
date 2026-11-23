"""Exercise sequence authoring on a disposable copy of a root-motion sequence."""

import argparse
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--sequence", required=True, help="Disposable animation under /Game")
    parser.add_argument("--settings", required=True, help="UniformIndexable compression package")
    args = parser.parse_args()
    client = Client(args.project, args.engine)

    def request(action, **kwargs):
        result = client.execute(dict(action=action, asset=args.sequence, **kwargs))
        assert result["ok"], result.get("error", result)
        return result

    before = request("asset_inspect")["revision"]
    edits = [
        {"op": "set_float_curve", "curve": "Stride", "keys": [{"time": 0, "value": 1}]},
        {"op": "apply_distance_curve", "replace_existing": True},
        {"op": "set_curve_compression", "settings": args.settings, "verify_indexed_curve": "Distance"},
    ]
    preview = request("asset_apply", expected_revision=before, operations=edits)
    assert preview["indexed_curve_samples"] > 1 and not preview["saved"]
    assert request("asset_inspect")["revision"] == before
    saved = request("asset_apply", expected_revision=before, operations=edits, dry_run=False)
    assert saved["saved"] and Path(saved["backups"][args.sequence]).is_file()
    animation = request("asset_inspect")["animation"]
    assert animation["float_curves"]["Stride"] == [{"time": 0, "value": 1}]
    distance = animation["float_curves"]["Distance"]
    assert len(distance) > 1 and distance[0]["value"] != distance[-1]["value"], distance
    assert animation["codec"].endswith("UniformIndexable"), animation["codec"]
    print(f"{args.sequence}: {len(distance)} keys and {saved['indexed_curve_samples']} compressed samples")

    montage = "/Game/__UAssetAgentTests/AM_" + uuid.uuid4().hex
    create = {"action": "asset_create", "asset": montage, "class": "/Script/Engine.AnimMontage"}
    invalid = client.execute(dict(create, montage={"sequence": args.sequence, "slot": "NoSuchSlot"}))
    assert not invalid["ok"] and not invalid["saved"], invalid
    options = {"sequence": args.sequence, "slot": "DefaultSlot"}
    preview = client.execute(dict(create, montage=options))
    assert preview["ok"] and not preview["saved"], preview
    saved = client.execute(dict(create, montage=options, dry_run=False))
    assert saved["ok"] and saved["saved"], saved
    reopened = client.execute({"action": "asset_inspect", "asset": montage})["montage"]
    assert reopened == saved["montage"] and reopened["root_motion"]
    assert reopened["length"] == animation["length"] and reopened["skeleton"] == animation["skeleton"]
    track, = reopened["tracks"]
    assert track == {"slot": "DefaultSlot", "sequences": [args.sequence + "." + args.sequence.rsplit("/", 1)[1]]}
    print(f"{montage}: verified slot, skeleton, source, duration and root motion after reload")


if __name__ == "__main__":
    main()
