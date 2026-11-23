"""Verify typed message listener, cancellation and delegate wiring across UE processes."""

import argparse
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from uasset_agent import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, help="Disposable project with Gameplay Message Router enabled")
    parser.add_argument("--engine", required=True)
    parser.add_argument("--tag", required=True, help="A registered gameplay message channel")
    args = parser.parse_args()
    client = Client(args.project, args.engine)
    asset = "/Game/__UAssetAgentTests/Async_" + uuid.uuid4().hex + "/W_QuickCommFixture"

    def request(action, **kwargs):
        response = client.execute(dict(action=action, asset=asset, **kwargs))
        assert response["ok"], response
        return response

    def add(node_id, kind, **fields):
        return dict(op="add_node", id=node_id, kind=kind, **fields)

    def connect(source, output, target, input_pin):
        return {"op": "connect", "from": {"node": source, "pin": output}, "to": {"node": target, "pin": input_pin}}

    payload = "/Script/Engine.HitResult"
    proxy = "/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"
    fixture = request("create", kind="widget", parent_class="/Script/UMG.UserWidget", operations=[], dry_run=False)
    operations = [
        {"op": "add_variable", "name": "Listener", "type": {"category": "object", "path": proxy}},
        add("start", "custom_event", name="StartListening"),
        add("stop", "custom_event", name="StopListening"),
        add("receive", "custom_event", name="ReceiveMessage", inputs=[{"name": "Message", "type": {"category": "struct", "path": payload}}]),
        add("visibility", "custom_event", name="HandleVisibility", inputs=[
            {"name": "InVisibility", "type": {"category": "enum", "path": "/Script/UMG.ESlateVisibility"}},
        ]),
        add("listen", "gameplay_message_listener", channel=args.tag, payload_type=payload),
        add("store", "set", variable="Listener"),
        add("get", "get", variable="Listener"),
        add("clear", "set", variable="Listener"),
        add("cancel", "call", **{"class": "/Script/Engine.CancellableAsyncAction", "function": "Cancel"}),
        add("valid", "call", **{"class": "/Script/Engine.KismetSystemLibrary", "function": "IsValid"}),
        add("branch", "branch"),
        add("dispatch", "call", function="ReceiveMessage"),
        add("callback", "create_delegate"),
        add("bind", "bind_delegate", delegate="OnVisibilityChanged"),
        add("unbind", "unbind_delegate", delegate="OnVisibilityChanged"),
        connect("start", "then", "listen", "execute"),
        connect("listen", "then", "store", "execute"),
        connect("listen", "AsyncTaskProxy", "store", "Listener"),
        connect("listen", "OnMessageReceived", "dispatch", "execute"),
        connect("listen", "Payload", "dispatch", "Message"),
        connect("store", "then", "bind", "execute"),
        connect("callback", "OutputDelegate", "bind", "Delegate"),
        connect("callback", "OutputDelegate", "unbind", "Delegate"),
        {"op": "set_delegate_function", "node": "callback", "function": "HandleVisibility"},
        connect("stop", "then", "unbind", "execute"),
        connect("unbind", "then", "branch", "execute"),
        connect("get", "Listener", "valid", "Object"),
        connect("valid", "ReturnValue", "branch", "Condition"),
        connect("branch", "then", "cancel", "execute"),
        connect("get", "Listener", "cancel", "self"),
        connect("cancel", "then", "clear", "execute"),
    ]
    preview = request("apply", expected_revision=fixture["revision"], operations=operations, dry_run=True)
    assert not preview["saved"]
    assert request("inspect", response_mode="summary")["revision"] == fixture["revision"]
    applied = request("apply", expected_revision=fixture["revision"], operations=operations, dry_run=False)
    assert applied["saved"] and Path(applied["backup"]).is_file()
    graph = request("inspect", response_mode="graph", graph="EventGraph")["blueprint"]["graphs"][0]
    nodes = {node["id"]: node for node in graph["nodes"]}
    listener = nodes[applied["node_ids"]["listen"]]
    pins = {pin["name"]: pin for pin in listener["pins"]}
    assert listener["class"].endswith("K2Node_AsyncAction_ListenForGameplayMessages"), listener
    assert args.tag in pins["Channel"]["default"], pins["Channel"]
    assert pins["Payload"]["type"]["path"] == payload and pins["Payload"]["links"], pins["Payload"]
    assert pins["AsyncTaskProxy"]["links"] and pins["OnMessageReceived"]["links"], pins
    for alias, class_name in {"cancel": "K2Node_CallFunction", "bind": "K2Node_AddDelegate", "unbind": "K2Node_RemoveDelegate"}.items():
        node = nodes[applied["node_ids"][alias]]
        assert node["class"].endswith(class_name) and any(pin["links"] for pin in node["pins"]), node
    print("Gameplay message payload, proxy cancellation and multicast delegate wiring survived widget compilation and reopening.")


if __name__ == "__main__":
    main()
