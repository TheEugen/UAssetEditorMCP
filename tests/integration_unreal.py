"""End-to-end checks requiring the compiled plugin in a disposable UE 5.4 project.

Creates and retains /Game/UAssetAgentTests/BP_<unique-id> for manual inspection.
Never run against a shipping project's assets. Uses the real commandlet process.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client, ROOT


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    client = Client(args.project, args.engine, timeout=600)
    name = "BP_" + uuid.uuid4().hex[:12]
    folder = "/Game/UAssetAgentTests/" + name
    asset = folder + "/BP_Main"
    filename = client.project.parent / "Content" / (asset.removeprefix("/Game/") + ".uasset")

    def request(value, success=True):
        result = client.execute(value)
        check(result["ok"] == success, json.dumps(result, indent=2))
        return result

    creation = json.loads((ROOT / "examples/create_replicated_actor.json").read_text(encoding="utf-8"))
    creation["asset"] = asset
    creation["operations"].extend([
        {"op": "add_function", "name": "EchoValue", "entry_id": "entry", "result_id": "result",
         "inputs": [{"name": "InValue", "type": {"category": "int"}}],
         "outputs": [{"name": "OutValue", "type": {"category": "int"}}]},
        {"op": "connect", "graph": "EchoValue", "from": {"node": "entry", "pin": "InValue"},
         "to": {"node": "result", "pin": "OutValue"}}
    ])
    dry = request(dict(creation, dry_run=True))
    check(not dry["saved"] and not filename.exists(), "Dry-run created an asset file")
    print("PASS dry-run compilation without an asset write", flush=True)

    created = request(creation)
    check(created["saved"] and filename.is_file(), "Create did not save a real .uasset")
    check(created["revision"] == hashlib.sha1(filename.read_bytes()).hexdigest(), "Incorrect file revision")
    print("PASS real asset creation and revision", flush=True)

    inspect = request({"action": "inspect", "asset": asset})
    bp = inspect["blueprint"]
    health = next(v for v in bp["variables"] if v["name"] == "Health")
    check(health["replicated"] and health["rep_notify"] == "OnRep_Health", "Replication metadata did not persist")
    check(float(bp["defaults"]["Health"]) == 100, "Health default did not persist")
    check(bp["defaults"]["bReplicates"].lower() == "true", "Actor replication is disabled")
    graphs = {g["name"]: g for g in bp["graphs"]}
    check("OnRep_Health" in graphs and "EchoValue" in graphs, "Function graphs missing")
    event = next(n for n in graphs["EventGraph"]["nodes"] if n.get("event") == "ServerResetHealth")
    check(event["replication"] == "server" and event["reliable"], "RPC flags did not persist")
    flow = next(p for p in event["pins"] if p["name"] == "then")
    check(any(l["node"] == created["node_ids"]["set_health"] for l in flow["links"]), "Execution connection did not persist")
    print("PASS reloaded graph, RPC, variables and defaults", flush=True)

    summary = request({"action": "inspect", "asset": asset, "response_mode": "summary"})
    check(summary["revision"] == inspect["revision"], "Summary lost the revision")
    check("defaults" not in summary["blueprint"], "Summary includes class defaults")
    check(all("nodes" not in g for g in summary["blueprint"]["graphs"]), "Summary includes full nodes")
    check(len(json.dumps(summary)) < len(json.dumps(inspect)) / 2, "Summary did not substantially reduce output")
    event_summary = next(g for g in summary["blueprint"]["graphs"] if g["name"] == "EventGraph")
    rpc = next(e for e in event_summary["events"] if e["event"] == "ServerResetHealth")
    check(rpc["replication"] == "server" and rpc["reliable"], "Summary lost RPC flags")
    for selector in ("EchoValue", graphs["EchoValue"]["path"]):
        selected = request({"action": "inspect", "asset": asset, "response_mode": "graph", "graph": selector})
        check(selected["blueprint"]["graphs"] == [graphs["EchoValue"]], "Graph selection returned other graphs or lost nodes")
        check("defaults" not in selected["blueprint"] and "variables" not in selected["blueprint"], "Graph response includes unrelated fields")
    request({"action": "inspect", "asset": asset, "response_mode": "graph", "graph": "MissingGraph"}, success=False)
    print("PASS summary size, RPC metadata and graph selection", flush=True)

    functions = request({"action": "functions", "class": "/Script/Engine.KismetSystemLibrary", "search": "PrintString"})
    check(any(f["name"] == "PrintString" for f in functions["functions"]), "Function reflection failed")
    listing = request({"action": "list", "path": "/Game/UAssetAgentTests"})
    check(asset in listing["assets"], "Saved Blueprint missing from asset registry")

    original = filename.read_bytes()
    for path in (asset, folder):
        compiled = request({"action": "compile", "path": path})
        check(compiled["checked"] == 1 and compiled["passed"] == 1 and compiled["failed"] == 0, "Incorrect compile counts")
        check(compiled["failures"] == [] and "blueprint" not in compiled and not compiled["saved"], "Compile returned graphs or saved")
        check(filename.read_bytes() == original, "Compile modified an asset")
    empty = request({"action": "compile", "path": folder + "/Missing"}, success=False)
    check(empty["checked"] == 0, "Unmatched path passed silently")
    change = {"action": "apply", "asset": asset, "expected_revision": inspect["revision"],
              "operations": [{"op": "set_default", "property": "Health", "value": "75.0"}]}
    compact_dry = request(dict(change, dry_run=True, response_mode="summary"))
    check("defaults" not in compact_dry["blueprint"] and "diagnostics" in compact_dry, "Compact dry run lost diagnostics")
    check(filename.read_bytes() == original, "Dry-run modified an existing asset")

    # One valid mutation followed by an invalid operation must not persist either.
    invalid = copy.deepcopy(change)
    invalid["operations"].append({"op": "set_pin_default", "node": "missing", "pin": "InString", "value": "oops"})
    error = request(invalid, success=False)
    check(error.get("operation_index") == 1 and filename.read_bytes() == original, "Failed batch modified the asset")

    # A function with a required ActorComponent target cannot use an Actor as self.
    compile_error = dict(change, operations=[
        {"op": "add_node", "id": "bad_event", "kind": "custom_event", "name": "TestInvalidTarget"},
        {"op": "add_node", "id": "bad_call", "kind": "call", "class": "/Script/Engine.ActorComponent", "function": "K2_DestroyComponent"},
        {"op": "connect", "from": {"node": "bad_event", "pin": "then"}, "to": {"node": "bad_call", "pin": "execute"}}
    ])
    error = request(compile_error, success=False)
    check(any(d["severity"] == "error" for d in error.get("diagnostics", [])), "No compiler diagnostic for invalid target")
    check(filename.read_bytes() == original, "Compilation failure modified the asset")
    print("PASS failed operation/compile and dry-run preserve original bytes", flush=True)

    # Invalid response selection must fail before saving a valid mutation.
    request(dict(change, response_mode="graph", graph="MissingGraph"), success=False)
    check(filename.read_bytes() == original, "Invalid graph selection saved a mutation")
    changed = request(dict(change, response_mode="summary"))
    check(changed["saved"] and Path(changed["backup"]).read_bytes() == original, "Backup does not match original")
    after = filename.read_bytes()
    request(change, success=False)
    check(filename.read_bytes() == after, "Revision conflict changed the asset")
    request(creation, success=False)
    check(filename.read_bytes() == after, "Duplicate create changed the asset")
    final = request({"action": "inspect", "asset": asset})
    check(float(final["blueprint"]["defaults"]["Health"]) == 75, "Changed default did not persist")
    print("PASS backup, revision conflict and overwrite protection", flush=True)

    # Create a real stale function reference, the regression this compile tool
    # is intended to catch. All fixtures stay inside this run's unique folder.
    parent_asset = folder + "/BP_Parent"
    parent_class = parent_asset + ".BP_Parent_C"
    parent = request({"action": "create", "asset": parent_asset, "response_mode": "summary", "operations": [
        {"op": "add_node", "kind": "custom_event", "id": "removed", "name": "RemovedFunction"}]})
    child_asset = folder + "/BP_Child"
    request({"action": "create", "asset": child_asset, "parent_class": parent_class,
             "response_mode": "summary", "operations": [
        {"op": "add_node", "kind": "custom_event", "id": "trigger", "name": "TriggerCall"},
        {"op": "add_node", "kind": "call", "id": "call", "class": parent_class, "function": "RemovedFunction"},
        {"op": "connect", "from": {"node": "trigger", "pin": "then"}, "to": {"node": "call", "pin": "execute"}}]})
    request({"action": "apply", "asset": parent_asset, "expected_revision": parent["revision"],
             "response_mode": "summary", "operations": [
        {"op": "delete_node", "node": parent["node_ids"]["removed"]}]})
    snapshots = {p: p.read_bytes() for p in filename.parent.glob("*.uasset")}
    failed = request({"action": "compile", "path": folder}, success=False)
    check(failed["checked"] == 3 and failed["passed"] == 2 and failed["failed"] == 1, "Batch did not continue after failure")
    check([f["asset"] for f in failed["failures"]] == [child_asset], "Compile did not return only the failing Blueprint")
    check(any(d["severity"] == "error" for d in failed["failures"][0]["diagnostics"]), "Compile lost failure diagnostics")
    check(all(p.read_bytes() == data for p, data in snapshots.items()), "Failed batch compile modified assets")
    print("PASS batch compilation, stale function diagnostics and no asset writes", flush=True)
    check_editing_extensions(client, request, folder + "/Editing")
    print(f"All integration checks passed. Retained test asset: {asset}", flush=True)


def check_editing_extensions(client, request, folder):
    asset = folder + "/BP_Edit"
    created = request({"action": "create", "asset": asset, "operations": [
        {"op": "add_variable", "name": "UnusedA", "type": {"category": "int"}, "replicated": True},
        {"op": "add_variable", "name": "UnusedB", "type": {"category": "int"}, "replicated": True},
        {"op": "add_node", "id": "rpc", "kind": "custom_event", "name": "OldMulticast",
         "replication": "multicast", "reliable": True}]})
    filename = client.project.parent / "Content" / (asset.removeprefix("/Game/") + ".uasset")
    before = filename.read_bytes()
    change = {"action": "apply", "asset": asset, "expected_revision": created["revision"], "operations": [
        {"op": "reparent", "parent_class": "/Script/Engine.Pawn"},
        {"op": "edit_custom_event", "node": created["node_ids"]["rpc"], "replication": "none",
         "inputs": [{"name": "Count", "type": {"category": "int"}}]},
        {"op": "delete_variable", "name": "UnusedA"},
        {"op": "delete_variable", "name": "UnusedB"}]}
    request(dict(change, dry_run=True))
    check(filename.read_bytes() == before, "Editing dry run modified the file")
    saved = request(change)
    check(Path(saved["backup"]).read_bytes() == before, "Editing backup is incorrect")
    inspected = request({"action": "inspect", "asset": asset})
    bp = inspected["blueprint"]
    check(bp["parent_class"] == "/Script/Engine.Pawn", "New parent did not persist")
    check(not bp["variables"], "Deleted replicated variables persisted")
    event = next(n for g in bp["graphs"] for n in g["nodes"] if n.get("event") == "OldMulticast")
    check(event["replication"] == "none" and not event["reliable"], "RPC flags did not clear")
    check(any(p["name"] == "Count" for p in event["pins"]), "Event parameter did not persist")
    after = filename.read_bytes()
    for operation in [
            {"op": "reparent", "parent_class": asset + ".BP_Edit_C"},
            {"op": "delete_variable", "name": "bReplicates"},
            {"op": "edit_custom_event", "node": event["id"], "reliable": True}]:
        request(dict(change, operations=[operation], expected_revision=inspected["revision"]), success=False)
        check(filename.read_bytes() == after, "Rejected edit modified the file")

    native_asset = folder + "/BP_NativeComponents"
    native = request({"action": "create", "asset": native_asset, "parent_class": "/Script/Engine.Character", "operations": []})
    components = request({"action": "components", "asset": native_asset})
    check(components["revision"] == native["revision"] and "blueprint" not in components, "Component inspection lost revision or returned graphs")
    capsule = next(c for c in components["components"] if c["id"] == "native:CapsuleComponent")
    native_file = client.project.parent / "Content" / (native_asset.removeprefix("/Game/") + ".uasset")
    native_before = native_file.read_bytes()
    component_edit = {"action": "apply", "asset": native_asset, "expected_revision": native["revision"],
                      "operations": [{"op": "set_component_default", "component": capsule["id"], "property": "CapsuleRadius", "value": "55"}]}
    request(dict(component_edit, dry_run=True))
    check(native_file.read_bytes() == native_before, "Component dry run saved")
    request(component_edit)
    reloaded = request({"action": "components", "asset": native_asset})
    capsule = next(c for c in reloaded["components"] if c["id"] == "native:CapsuleComponent")
    check(float(capsule["defaults"]["CapsuleRadius"]) == 55, "Component default did not survive save/reload")
    redirectors = request({"action": "redirectors", "path": folder})
    check(redirectors["redirectors"] == [] and not redirectors["saved"], "Redirector listing returned ordinary Blueprints")
    print("PASS persisted reparenting, RPC/parameter edits, variable deletion and native component defaults", flush=True)


if __name__ == "__main__":
    main()
