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
    asset = "/Game/UAssetAgentTests/" + name
    filename = client.project.parent / "Content/UAssetAgentTests" / (name + ".uasset")

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

    functions = request({"action": "functions", "class": "/Script/Engine.KismetSystemLibrary", "search": "PrintString"})
    check(any(f["name"] == "PrintString" for f in functions["functions"]), "Function reflection failed")
    listing = request({"action": "list", "path": "/Game/UAssetAgentTests"})
    check(asset in listing["assets"], "Saved Blueprint missing from asset registry")

    original = filename.read_bytes()
    change = {"action": "apply", "asset": asset, "expected_revision": inspect["revision"],
              "operations": [{"op": "set_default", "property": "Health", "value": "75.0"}]}
    request(dict(change, dry_run=True))
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

    changed = request(change)
    check(changed["saved"] and Path(changed["backup"]).read_bytes() == original, "Backup does not match original")
    after = filename.read_bytes()
    request(change, success=False)
    check(filename.read_bytes() == after, "Revision conflict changed the asset")
    request(creation, success=False)
    check(filename.read_bytes() == after, "Duplicate create changed the asset")
    final = request({"action": "inspect", "asset": asset})
    check(float(final["blueprint"]["defaults"]["Health"]) == 75, "Changed default did not persist")
    print("PASS backup, revision conflict and overwrite protection", flush=True)
    print(f"All integration checks passed. Retained test asset: {asset}", flush=True)


if __name__ == "__main__":
    main()
