"""Create, save, reload and validate a Function Library and its caller in UE 5.4."""
import argparse
import json
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def check(condition, detail):
    if not condition:
        raise AssertionError(json.dumps(detail, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--engine", required=True)
    args = parser.parse_args()
    client = Client(args.project, args.engine, timeout=240)
    root = "/Game/__UAssetAgentTests/FunctionLibrary_" + uuid.uuid4().hex
    library = root + "/BFL_Test"
    caller = root + "/BP_Caller"
    created = []

    def request(action, ok=True, **args):
        result = client.execute(dict(action=action, **args))
        check(result["ok"] == ok, result)
        return result

    def inspect(asset, **options):
        return request("inspect", asset=asset, **options)

    def create(asset, operations, **options):
        result = request("create", asset=asset, operations=operations, response_mode="summary", **options)
        check(result["saved"], result)
        created.append(asset)
        return result

    def apply(operations, rev, *, dry_run=True, ok=True):
        return request("apply", ok=ok, asset=library, expected_revision=rev, operations=operations,
                       dry_run=dry_run, response_mode="summary")

    integer = {"category": "int"}
    count = {"name": "Count", "type": integer}
    value = {"name": "Value", "type": integer}
    library_operations = [
        {"op": "add_function", "name": "EchoCount", "pure": True, "inputs": [count],
         "outputs": [value], "entry_id": "entry", "result_id": "result"},
        {"op": "connect", "graph": "EchoCount", "from": {"node": "entry", "pin": "Count"},
         "to": {"node": "result", "pin": "Value"}},
        {"op": "add_function", "name": "UnusedWork", "pure": False},
    ]
    try:
        dry = request("create", asset=library, kind="function_library", operations=library_operations, dry_run=True)
        check(not dry["saved"], dry)
        create(library, library_operations, kind="function_library")
        snapshot = inspect(library, response_mode="summary")
        info = snapshot["blueprint"]
        check(info["kind"] == "function_library", snapshot)
        check(info["generated_class"] == library + ".BFL_Test_C", info)
        signatures = {f["name"]: f for f in info["functions"]}
        echo = signatures["EchoCount"]
        check(echo["pure"] and echo["static"] and not signatures["UnusedWork"]["pure"], signatures)
        user_parameters = [p for p in echo["parameters"] if p["name"] != "__WorldContext"]
        check([(p["name"], p["type"]["category"], p["output"]) for p in user_parameters]
              == [("Count", "int", False), ("Value", "int", True)], echo)
        details = inspect(library, response_mode="graph", graph="EchoCount")["blueprint"]
        check(len(details["graphs"]) == 1 and details["functions"] == [echo], details)
        reflected = request("functions", **{"class": info["generated_class"], "search": "EchoCount"})
        check(reflected["functions"] == [echo], reflected)

        revised = snapshot["revision"]
        reject = apply([{"op": "add_function", "name": "ValidFunction"},
                        {"op": "add_variable", "name": "Instance", "type": integer}], revised, ok=False)
        check("not supported in function libraries" in reject["error"] and not reject["saved"]
              and reject["operation_index"] == 1, reject)
        reject = apply([{"op": "add_node", "graph": "EchoCount", "kind": "custom_event",
                         "name": "Event", "id": "event"}], revised, ok=False)
        check("Node kind is not supported" in reject["error"] and not reject["saved"], reject)

        created_caller = create(caller, [
            {"op": "add_function", "name": "ReadCount", "outputs": [{"name": "Result", "type": integer}],
             "result_id": "result"},
            {"op": "add_node", "kind": "call", "graph": "ReadCount", "id": "call",
             "class": info["generated_class"], "function": "EchoCount"},
            {"op": "set_pin_default", "graph": "ReadCount", "node": "call", "pin": "Count", "value": 42},
            {"op": "connect", "graph": "ReadCount", "from": {"node": "call", "pin": "Value"},
             "to": {"node": "result", "pin": "Result"}},
        ])
        caller_revision = created_caller["revision"]
        request("compile", path=caller)

        additive = [
            {"op": "edit_function", "name": "EchoCount", "inputs": [count, {"name": "Extra", "type": integer}]},
            {"op": "edit_function", "name": "UnusedWork", "outputs": [value], "result_id": "unused_result"},
            {"op": "set_pin_default", "graph": "UnusedWork", "node": "unused_result", "pin": "Value", "value": 23},
        ]
        result = apply(additive, revised)
        affected = result["affected_callers"]
        check(not result["saved"] and len(affected) == 1 and affected[0]["asset"] == caller
              and affected[0]["functions"] == ["EchoCount"] and affected[0]["compiled"] and affected[0]["ok"], result)
        check(inspect(library, response_mode="summary")["revision"] == revised, result)
        result = apply(additive, revised, dry_run=False)
        check(result["saved"] and result["backup"], result)
        unused = next(f for f in result["blueprint"]["functions"] if f["name"] == "UnusedWork")
        check(any(p["name"] == "Value" and p["output"] for p in unused["parameters"]), unused)
        revised = result["revision"]
        graph = inspect(library, response_mode="graph", graph="EchoCount")["blueprint"]["graphs"][0]
        entry = next(n for n in graph["nodes"] if n["class"].endswith("K2Node_FunctionEntry"))
        entry_pins = {p["name"]: p for p in entry["pins"]}
        check(entry_pins["Count"]["links"] and "Extra" in entry_pins, graph)
        request("compile", path=caller)
        check(inspect(caller, response_mode="summary")["revision"] == caller_revision, "Caller file changed")

        for operation in [{"op": "delete_function", "name": "EchoCount"},
                          {"op": "edit_function", "name": "EchoCount", "pure": False}]:
            rejected = apply([operation], revised, dry_run=False, ok=False)
            checked = rejected["affected_callers"]
            check(not rejected["saved"] and checked[0]["asset"] == caller
                  and checked[0]["incompatible_functions"] == ["EchoCount"] and "compiled" in checked[0], rejected)
            check(inspect(library, response_mode="summary")["revision"] == revised, rejected)

        result = apply([{"op": "delete_function", "name": "UnusedWork"}], revised, dry_run=False)
        check(result["saved"] and result["affected_callers"] == [], result)
        names = {f["name"] for f in inspect(library, response_mode="summary")["blueprint"]["functions"]}
        check("EchoCount" in names and "UnusedWork" not in names, sorted(names))
        print("PASS Function Library creation, static signatures, graph edit, saved caller and caller checks", flush=True)
    finally:
        for asset in reversed(created):
            revision = inspect(asset, response_mode="summary")["revision"]
            request("delete_asset", asset=asset, expected_revision=revision, dry_run=False)


if __name__ == "__main__":
    main()
