"""UE 5.4 commandlet tests: create/dry-run/save/reload/delete temporary AnimBPs/WBPs.

Requires the rebuilt plugin in a disposable project. Never edits existing assets.
Every call launches a fresh process, so persistence assertions cannot accidentally
pass using the in-memory objects from the edit. Native automation covers fixture
anim nodes, valid thread-safe function signatures and redirector creation/fixup.
"""
import argparse
import json
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def check(value, message):
    if not value:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    client = Client(args.project, args.engine, timeout=600)
    root = "/Game/__UAssetAgentTests/Process_" + uuid.uuid4().hex
    created = []

    def request(value, success=True):
        result = client.execute(value)
        check(result["ok"] == success, json.dumps(result, indent=2))
        check(result.get("protocol_version") == 2, "Install protocol 2 before running this suite")
        if success:
            check(not any(d["severity"] == "error" for d in result.get("diagnostics", [])), "Compiler errors: " + json.dumps(result))
        return result

    def inspect(asset, **options):
        return request(dict(action="inspect", asset=asset, **options))

    def create(name, kind, operations):
        asset = root + "/" + name
        req = dict(action="create", asset=asset, kind=kind, template=kind == "animation",
                   parent_class="/Script/Engine.AnimInstance" if kind == "animation" else "/Script/UMG.UserWidget",
                   operations=operations, response_mode="summary")
        dry = request(dict(req, dry_run=True))
        check(not dry["saved"], "Create dry run saved")
        saved = request(dict(req, dry_run=False))
        created.append(asset)
        check(saved["saved"], "Creation failed to save")
        return asset

    def apply(asset, operations, success=True):
        before = inspect(asset, response_mode="summary")["revision"]
        req = dict(action="apply", asset=asset, operations=operations, expected_revision=before, response_mode="summary")
        dry = request(dict(req, dry_run=True), success)
        check(not dry["saved"], "Apply dry run saved")
        check(inspect(asset, response_mode="summary")["revision"] == before, "Dry run changed the file")
        if not success:
            return dry
        saved = request(dict(req, dry_run=False))
        check(saved["saved"] and Path(saved["backup"]).is_file(), "No save/backup")
        request(dict(req, expected_revision="0" * 40), success=False)
        check(inspect(asset, response_mode="summary")["revision"] == saved["revision"], "Revision mismatch after reload")
        return saved

    try:
        variable = {"op": "add_variable", "name": "SharedValue", "type": {"category": "int"}}
        parent = create("ABP_Parent", "animation", [variable])
        child = create("ABP_Child", "animation", [variable,
            {"op": "add_node", "kind": "get", "variable": "SharedValue", "id": "getter"},
            {"op": "add_function", "name": "UnusedFunction"},
            {"op": "add_node", "kind": "custom_event", "name": "OldLogic", "id": "old"}])
        snapshot = inspect(child)["blueprint"]
        event_graph = next(g for g in snapshot["graphs"] if g["name"] == "EventGraph")
        getter = next(n for n in event_graph["nodes"] if n["class"].endswith("K2Node_VariableGet"))
        old = next(n for n in event_graph["nodes"] if n.get("event") == "OldLogic")
        apply(child, [
            {"op": "reparent", "parent_class": parent + ".ABP_Parent_C"},
            {"op": "delete_variable", "name": "SharedValue", "retarget_references": True},
            {"op": "delete_function", "name": "UnusedFunction"},
            {"op": "delete_node", "node": old["id"]},
            {"op": "set_default", "property": "bReceiveNotifiesFromLinkedInstances", "value": "True"}])
        after = inspect(child)["blueprint"]
        check(not any(v["name"] == "SharedValue" for v in after["variables"]), "Duplicate variable persisted")
        check(any(n["id"] == getter["id"] for g in after["graphs"] for n in g["nodes"]), "Retarget deleted getter")
        check(after["defaults"]["bReceiveNotifiesFromLinkedInstances"].lower() == "true", "Inherited default lost")
        anim = inspect(parent, response_mode="graph", graph="AnimGraph")["blueprint"]
        node = next(n for n in anim["graphs"][0]["nodes"] if "functions" in n)
        apply(parent, [
            {"op": "set_anim_node_function", "graph": "AnimGraph", "node": node["id"], "slot": "UpdateFunction", "function": None},
            {"op": "set_anim_node_property", "graph": "AnimGraph", "node": node["id"], "property": "Tag", "value": "AgentTest"}])
        anim = inspect(parent, response_mode="graph", graph="AnimGraph")["blueprint"]
        node = next(n for n in anim["graphs"][0]["nodes"] if n["id"] == node["id"])
        check(node["functions"]["UpdateFunction"] is None, "Binding clear did not persist")
        check(node["properties"]["Tag"] == "AgentTest", "Anim property did not persist")
        print("PASS animation migration, retargeting, defaults and graph inspection", flush=True)

        widget_parent = create("W_Parent", "widget", [])
        widget = create("W_Test", "widget", [
            {"op": "add_widget", "id": "root", "class": "/Script/UMG.Overlay", "name": "Root"}])
        ops = [{"op": "add_widget", "id": "entry" + str(i), "name": "Entry" + str(i),
                "class": "/Script/UMG.Overlay", "parent": "Root", "index": i,
                "slot": {"Padding": "(Left=4,Top=0,Right=4,Bottom=0)"}} for i in range(6)]
        ops += [
            {"op": "rename_widget", "widget": "entry4", "name": "Chainshot"},
            {"op": "rename_widget", "widget": "entry5", "name": "GreekFire"},
            {"op": "move_widget", "widget": "entry5", "parent": "Root", "index": 4},
            {"op": "move_widget", "widget": "entry5", "parent": "Root", "index": 5},
            {"op": "set_widget_property", "widget": "entry4", "property": "Visibility", "value": "Hidden"},
            {"op": "set_slot_property", "widget": "entry4", "property": "Padding", "value": "(Left=8,Top=0,Right=8,Bottom=0)"},
            {"op": "add_function", "name": "TestQuickComm", "inputs": [{"name": "Choice", "type": {"category": "int"}}]},
        ]
        for i in range(3):
            ops += [
                {"op": "add_widget", "class": "/Script/UMG.Button", "id": "btn" + str(i), "name": "Button" + str(i), "parent": "Root"},
                {"op": "bind_widget_event", "widget": "btn" + str(i), "event": "OnClicked", "id": "clicked" + str(i)},
                {"op": "add_node", "kind": "call", "class": "self", "function": "TestQuickComm", "id": "call" + str(i)},
                {"op": "set_pin_default", "node": "call" + str(i), "pin": "Choice", "value": str(i)},
                {"op": "connect", "from": {"node": "clicked" + str(i), "pin": "then"}, "to": {"node": "call" + str(i), "pin": "execute"}},
            ]
        ops.append({"op": "reparent", "parent_class": widget_parent + ".W_Parent_C"})
        saved = apply(widget, ops)
        snapshot = inspect(widget)["blueprint"]
        tree = {w["name"]: w for w in snapshot["widget_tree"]}
        check(tree["Chainshot"]["index"] == 4 and tree["GreekFire"]["index"] == 5, "Widget order did not survive save")
        check(tree["Chainshot"]["properties"]["Visibility"] == "Hidden", "Widget property lost")
        check("8.000000" in tree["Chainshot"]["slot"]["properties"]["Padding"], "Slot property lost")
        events = [n for g in snapshot["graphs"] for n in g["nodes"] if n.get("delegate") == "OnClicked"]
        check(len(events) == 3, "Delegate bindings did not persist")
        check(all(any(p["links"] for p in event["pins"]) for event in events), "Delegate event wiring missing")
        apply(widget, [{"op": "rename_widget", "widget": "Button0", "name": "UnsafeRename"}], success=False)
        apply(widget, [{"op": "delete_node", "node": saved["node_ids"]["clicked0"]},
                       {"op": "delete_node", "node": saved["node_ids"]["call0"]},
                       {"op": "remove_widget", "widget": "Button0"}])
        check("Button0" not in {w["name"] for w in inspect(widget)["blueprint"]["widget_tree"]}, "Removed widget persisted")
        summary = inspect(widget, response_mode="summary")["blueprint"]
        check(all("nodes" not in g for g in summary["graphs"]), "Summary contains full nodes")
        check(all("properties" not in w for w in summary["widget_tree"]), "Summary contains widget defaults")
        tags = apply(widget, [
            {"op": "add_function", "name": "TagFixture", "inputs": [
                {"name": "Phase", "type": {"category": "struct", "path": "/Script/GameplayTags.GameplayTag"}}]},
            {"op": "add_node", "kind": "call", "class": "self", "function": "TagFixture", "id": "phaseCall"},
            {"op": "set_pin_default", "node": "phaseCall", "pin": "Phase", "value": '(TagName="ShooterGame.GamePhase.PostGame")'},
        ])
        tag_node = tags["node_ids"]["phaseCall"]
        apply(widget, [{"op": "set_pin_default", "node": tag_node, "pin": "Phase", "value": '(TagName="PP.GamePhase.PostRound")'}])
        graph = inspect(widget, response_mode="graph", graph="EventGraph")["blueprint"]["graphs"][0]
        tag_call = next(n for n in graph["nodes"] if n["id"] == tag_node)
        check("PP.GamePhase.PostRound" in next(p["default"] for p in tag_call["pins"] if p["name"] == "Phase"), "Gameplay tag literal replacement did not persist")
        request({"action": "compile", "path": root})
        print("PASS widget tree/order/properties/delegates, dry runs, backups, revisions and reload", flush=True)

        # Deletion's referencer veto must reject the parent without touching it.
        result = request({"action": "delete_asset", "asset": parent, "dry_run": False}, success=False)
        check(child in result["referencers"] and not result["saved"], "Delete referencer check failed")
    finally:
        failures = []
        for asset in reversed(created):
            try:
                before = inspect(asset, response_mode="summary")["revision"]
                dry = request({"action": "delete_asset", "asset": asset, "expected_revision": before, "dry_run": True})
                check(not dry["saved"], "Delete dry run saved")
                deleted = request({"action": "delete_asset", "asset": asset, "expected_revision": before, "dry_run": False})
                check(deleted["deleted"] and deleted["backups"], "Deletion did not return backup")
            except Exception as exc:
                failures.append((asset, str(exc)))
        if failures:
            raise RuntimeError("Test fixture cleanup failed: " + json.dumps(failures))
    print("PASS temporary fixture cleanup", flush=True)


if __name__ == "__main__":
    main()
