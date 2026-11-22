"""Verify Blueprint declarations and graph nodes across fresh UE commandlet processes."""
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
    args = parser.parse_args()
    client = Client(args.project, args.engine)
    root = "/Game/__UAssetAgentTests/Authoring_" + uuid.uuid4().hex
    actor, macros, interface = [root + "/" + name for name in ("BP_Test", "BPM_Test", "BPI_Test")]

    def request(action, asset, **kwargs):
        result = client.execute(dict(action=action, asset=asset, **kwargs))
        if not result["ok"]:
            raise AssertionError({k: result.get(k) for k in ("error", "operation_index", "diagnostics", "logs")})
        return result

    integer = {"category": "int"}
    param = {"name": "Value", "type": integer}
    request("create", interface, kind="interface", operations=[
        {"op": "add_function", "name": "ReceiveValue", "inputs": [param]}], dry_run=False)
    request("create", macros, kind="macro_library", operations=[
        {"op": "add_macro", "name": "Pass", "inputs": [param], "outputs": [param],
         "entry_id": "entry", "result_id": "result"},
        {"op": "connect", "graph": "Pass", "from": {"node": "entry", "pin": "Value"},
         "to": {"node": "result", "pin": "Value"}}], dry_run=False)
    categories = {"Score": integer,
                  "Locations": {"category": "name", "container": "map", "value_type": integer},
                  "Target": {"category": "softobject", "path": "/Script/Engine.Actor"},
                  "TargetClass": {"category": "softclass", "path": "/Script/Engine.Actor"},
                  "TimerCallback": {"category": "delegate", "signature": "/Script/Engine.TimerDynamicDelegate__DelegateSignature"},
                  "OverlapSignal": {"category": "mcdelegate", "signature": "/Script/Engine.ActorBeginOverlapSignature__DelegateSignature"}}
    operations = [{"op": "add_variable", "name": name, "type": typ} for name, typ in categories.items()]
    operations += [
        {"op": "implement_interface", "interface": interface + ".BPI_Test_C"},
        {"op": "add_component", "class": "/Script/Engine.SceneComponent", "name": "Pivot"},
        {"op": "add_component", "class": "/Script/Engine.StaticMeshComponent", "name": "Mesh", "parent": "Pivot"},
        {"op": "add_node", "kind": "get", "id": "score", "variable": "Score"},
        {"op": "add_node", "kind": "macro", "id": "pass", "library": macros + ".BPM_Test", "macro": "Pass"},
        {"op": "connect", "from": {"node": "score", "pin": "Score"}, "to": {"node": "pass", "pin": "Value", "direction": "input"}},
        {"op": "add_node", "kind": "cast", "id": "cast", "class": "/Script/Engine.Pawn"},
        {"op": "add_node", "kind": "select", "id": "select", "index_type": {"category": "bool"}},
        {"op": "add_node", "kind": "select", "id": "channel", "index_type": {"category": "enum", "path": "/Script/Engine.ECollisionChannel"}},
        {"op": "add_node", "kind": "foreach", "id": "each"},
        {"op": "add_node", "kind": "make_struct", "id": "make", "struct": "/Script/Engine.WalkableSlopeOverride"},
        {"op": "add_node", "kind": "break_struct", "id": "break", "struct": "/Script/Engine.WalkableSlopeOverride"},
        {"op": "add_node", "kind": "timeline", "id": "time", "name": "Progress", "length": 2,
         "float_tracks": [{"name": "Alpha", "keys": [{"time": 0, "value": 0}, {"time": 2, "value": 1}]}]},
    ]
    before = request("create", actor, operations=operations, dry_run=False)
    child = root + "/BP_Child"
    request("create", child, parent_class=actor + ".BP_Test_C", operations=[
        {"op": "add_node", "kind": "get", "id": "inherited", "variable": "Score"},
        {"op": "add_node", "kind": "macro", "id": "pass", "library": macros + ".BPM_Test", "macro": "Pass"},
        {"op": "connect", "from": {"node": "inherited", "pin": "Score"}, "to": {"node": "pass", "pin": "Value"}},
    ], dry_run=False)
    blocked = client.execute({"action": "apply", "asset": actor, "expected_revision": before["revision"],
                              "dry_run": False, "operations": [
                                  {"op": "add_node", "kind": "get", "id": "component", "variable": "Mesh"},
                                  {"op": "remove_component", "component": "Mesh"}]})
    assert not blocked["ok"] and not blocked["saved"] and blocked["operation_index"] == 1
    edits = [{"op": "rename_variable", "name": "Score", "new_name": "Points"},
             {"op": "set_variable_replication", "name": "Points", "replicated": True, "condition": "COND_OwnerOnly"},
             {"op": "remove_component", "component": "Mesh"}]
    request("apply", actor, expected_revision=before["revision"], operations=edits, dry_run=True)
    assert request("inspect", actor)["revision"] == before["revision"]
    request("apply", actor, expected_revision=before["revision"], operations=edits, dry_run=False)
    bp = request("inspect", actor)["blueprint"]
    fields = {v["name"]: v for v in bp["variables"]}
    assert "Score" not in fields and fields["Points"]["replication_condition"] == "COND_OwnerOnly"
    assert fields["Locations"]["type"]["value_type"]["category"] == "int"
    assert fields["OverlapSignal"]["type"]["signature"].endswith("__DelegateSignature")
    assert fields["TimerCallback"]["type"]["signature"].endswith("TimerDynamicDelegate__DelegateSignature")
    assert fields["Target"]["type"]["category"] == "softobject"
    assert [v["name"] for v in bp["components"] if v["name"] in ("Pivot", "Mesh")] == ["Pivot"]
    assert bp["interfaces"] == [interface + ".BPI_Test_C"]
    assert bp["timelines"][0]["float_tracks"][0]["keys"][-1] == {"time": 2, "value": 1}
    nodes = [node for graph in bp["graphs"] for node in graph["nodes"]]
    assert any(p["name"] == "Index" and p["type"].get("path") == "/Script/Engine.ECollisionChannel"
               for node in nodes if node["class"].endswith("K2Node_Select") for p in node["pins"])
    getter = next(node for node in nodes if node.get("variable") == "Points")
    assert any(pin["name"] == "Points" and pin["links"] for pin in getter["pins"])
    assert request("inspect", macros)["blueprint"]["kind"] == "macro_library"
    assert request("inspect", interface)["blueprint"]["kind"] == "interface"
    child_bp = request("inspect", child)["blueprint"]
    inherited = [n for g in child_bp["graphs"] for n in g["nodes"] if n.get("variable") == "Points"]
    assert len(inherited) == 1 and any(p["links"] for p in inherited[0]["pins"])
    print("Graph authoring verified after save and reload.")


if __name__ == "__main__":
    main()
