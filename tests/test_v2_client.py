"""Protocol transport/validation tests; native semantics are tested in Unreal."""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import uasset_agent as agent


class V2ClientTests(unittest.TestCase):
    def test_specialized_creation_and_maintenance_validation(self):
        for request in [
            {"action": "create", "kind": "animation", "asset": "/Game/Test", "template": True, "operations": []},
            {"action": "create", "kind": "widget", "asset": "/Game/Test", "operations": []},
            {"action": "delete_asset", "asset": "/Game/Test", "force": False, "dry_run": True},
            {"action": "fixup_redirectors", "path": "/Game", "expected_revisions": {"/Game/A": "a" * 40}},
        ]:
            agent.validate_request(request)
        for request in [
            {"action": "create", "kind": [], "asset": "/Game/Test", "operations": []},
            {"action": "create", "kind": "animation", "asset": "/Game/Test", "operations": []},
            {"action": "delete_asset", "asset": "/Engine/Test"},
            {"action": "delete_asset", "asset": "/Game/Test", "force": "false"},
            {"action": "fixup_redirectors", "path": "/Game", "dry_run": "false"},
            {"action": "fixup_redirectors", "expected_revisions": {"/Game/A": "bad"}},
            {"action": "fixup_redirectors", "expected_revisions": {"/Engine/A": "a" * 40}},
        ]:
            with self.subTest(request=request), self.assertRaises(agent.AgentError):
                agent.validate_request(request)

    def test_mcp_passes_specialized_and_maintenance_arguments(self):
        captured = []

        class Client:
            def execute(self, request):
                agent.validate_request(request)
                captured.append(request)
                return {"ok": True, "protocol_version": 2, "saved": False}

        for action, args in [
            ("create", {"asset": "/Game/A", "kind": "animation", "template": True,
                        "parent_class": "/Script/Engine.AnimInstance", "operations": [], "response_mode": "summary"}),
            ("apply", {"asset": "/Game/W", "expected_revision": "a" * 40, "operations": [
                {"op": "bind_widget_event", "widget": "Btn", "event": "OnClicked", "id": "clicked"}]}),
            ("delete_asset", {"asset": "/Game/A", "dry_run": False, "expected_revision": "b" * 40}),
            ("fixup_redirectors", {"path": "/Game", "expected_revisions": {}, "dry_run": True}),
        ]:
            reply = agent.dispatch_rpc({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                "name": "blueprint_" + action, "arguments": args}}, Client())
            self.assertFalse(reply["result"]["isError"], reply)
            self.assertEqual(captured[-1], dict(args, action=action))
            self.assertEqual(json.loads(reply["result"]["content"][0]["text"])["protocol_version"], 2)

if __name__ == "__main__":
    unittest.main()
