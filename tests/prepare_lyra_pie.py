"""Prepare the Polynesien Lyra acceptance assets in a disposable project copy.

After preparation, run UnrealEditor-Cmd on that project with /Engine/Maps/Entry,
-UAssetAgentProjectPIE, "-ExecCmds=Automation RunTests UAssetAgent.ProjectPIE",
and "-TestExit=Automation Test Queue Empty". Use a Development Editor build.
"""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disposable-project", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--only", choices=("curves", "widget-and-montages"), help="Prepare one fixture group")
    args = parser.parse_args()
    client = Client(args.disposable_project, args.engine)

    def run(action, asset, **kwargs):
        result = client.execute(dict(action=action, asset=asset, **kwargs))
        assert result["ok"], result
        return result

    if args.only != "widget-and-montages":
        folder = "/Game/Polynesien/Characters/Animations/"
        sequences = [folder + "RangeWeapon/Musket/MM_Musket_Jog_" + side + "_Start"
                     for side in ("Fwd", "Bwd", "Left", "Right")]
        sequences.append(folder + "MeleeWeapon/MM_MeleeWeapon_Jog_Left_Start")
        settings = "/Game/Characters/Heroes/Mannequin/Animations/UniformIndexableCurveCompressionSettings"
        for sequence in sequences:
            original = run("asset_inspect", sequence)
            edits = [dict(op="apply_distance_curve", replace_existing=True),
                     dict(op="set_curve_compression", settings=settings, verify_indexed_curve="Distance")]
            run("asset_apply", sequence, expected_revision=original["revision"], operations=edits, dry_run=False)
        asset = "/Game/__UAssetAgentTests/PIE/ABP_Emote"
        package = client.project.parent / "Content" / (asset[6:] + ".uasset")
        if not package.exists():
            run("create", asset, kind="animation", parent_class="/Script/Engine.AnimInstance",
                skeleton="/Game/Characters/Heroes/Mannequin/Meshes/SK_Mannequin.SK_Mannequin", operations=[], dry_run=False)
        original = run("inspect", asset, response_mode="graph", graph="AnimGraph")
        nodes = original["blueprint"]["graphs"][0]["nodes"]
        if not any(n["class"].endswith("AnimGraphNode_LayeredBoneBlend") for n in nodes):
            root, = [n["id"] for n in nodes if n["class"].endswith("AnimGraphNode_Root")]
            run("apply", asset, expected_revision=original["revision"], dry_run=False, operations=[
                dict(op="add_node", graph="AnimGraph", kind="layered_blend_per_bone", id="emote",
                     layers=[dict(weight=1, branches=[dict(bone="clavicle_l", depth=1)])]),
                {"op": "connect", "graph": "AnimGraph", "from": {"node": "emote", "pin": "Pose"},
                 "to": {"node": root, "pin": "Result"}},
            ])
        print("Prepared five jog starts and a left-arm overlay.")
        if args.only == "curves":
            return

    widget = "/Game/Polynesien/UI/W_QuickComm"
    response = run("inspect", widget, response_mode="graph", graph="EventGraph")
    nodes = response["blueprint"]["graphs"][0]["nodes"]
    construct, = [node["id"] for node in nodes if node["title"] == "Event Construct"]
    proxy = "/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"
    payload = "/Script/LyraGame.QuickCommMessage"

    def add(alias, kind, **kwargs):
        return dict(op="add_node", id=alias, kind=kind, **kwargs)

    def link(source, output, target, input_pin):
        return {"op": "connect", "from": {"node": source, "pin": output}, "to": {"node": target, "pin": input_pin}}

    edits = [
        dict(op="add_variable", name="TestListener", type={"category": "object", "path": proxy}),
        dict(op="add_variable", name="TestCount", type={"category": "int"}),
        dict(op="add_variable", name="TestMessage", type={"category": "struct", "path": payload}),
        add("listen", "gameplay_message_listener", channel="PP.Player.QuickComm", payload_type=payload),
        add("store", "set", variable="TestListener"), add("read", "get", variable="TestListener"),
        add("clear", "set", variable="TestListener"), add("destruct", "event", event="Destruct"),
        add("cancel", "call", **{"class": "/Script/Engine.CancellableAsyncAction", "function": "Cancel"}),
        add("message", "set", variable="TestMessage"), add("count", "get", variable="TestCount"),
        add("inc", "call", **{"class": "/Script/Engine.KismetMathLibrary", "function": "Add_IntInt"}),
        add("record", "set", variable="TestCount"),
        dict(op="set_pin_default", node="inc", pin="B", value="1"),
        link(construct, "then", "listen", "execute"), link("listen", "then", "store", "execute"),
        link("listen", "AsyncTaskProxy", "store", "TestListener"), link("listen", "OnMessageReceived", "message", "execute"),
        link("listen", "Payload", "message", "TestMessage"), link("message", "then", "record", "execute"),
        link("count", "TestCount", "inc", "A"), link("inc", "ReturnValue", "record", "TestCount"),
        link("destruct", "then", "cancel", "execute"), link("read", "TestListener", "cancel", "self"),
        link("cancel", "then", "clear", "execute"),
    ]
    if not any(node["class"].endswith("K2Node_AsyncAction_ListenForGameplayMessages") for node in nodes):
        run("apply", widget, operations=edits, expected_revision=response["revision"], dry_run=False)
    compiled = client.execute({"action": "compile", "path": widget})
    assert compiled["ok"], compiled
    for side in ("Left", "Right"):
        sequence = "/Game/Characters/Heroes/Mannequin/Animations/Actions/MM_Dash_" + side
        montage = "/Game/__UAssetAgentTests/PIE/AM_" + side
        inspected = run("asset_inspect", sequence)
        if not inspected["animation"]["root_motion"]:
            run("asset_apply", sequence, expected_revision=inspected["revision"], dry_run=False,
                operations=[dict(op="set_property", property="bEnableRootMotion", value="True")])
        package = client.project.parent / "Content" / (montage[6:] + ".uasset")
        if package.exists():
            created = run("asset_inspect", montage)
        else:
            created = run("asset_create", montage, **{"class": "/Script/Engine.AnimMontage"},
                          montage={"sequence": sequence, "slot": "DefaultSlot"}, dry_run=False)
        assert created["montage"]["root_motion"], created["montage"]
    print("Prepared QuickComm and two directional root-motion montages in the disposable project.")


if __name__ == "__main__":
    main()
