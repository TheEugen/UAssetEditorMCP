import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import uasset_agent as agent


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "Project with spaces.uproject"
        self.project.write_text('{"FileVersion":3}', encoding="utf-8")
        self.editor = self.root / "UnrealEditor-Cmd.exe"
        self.editor.touch()
        self.client = agent.Client(self.project, self.editor)

    def test_validation_rejects_bad_requests_before_launch(self):
        for request in [[], {}, {"action": []}, {"action": "apply", "asset": "/Game/A", "operations": "bad"},
                        {"action": "inspect", "asset": "/Game/../A"},
                        {"action": "inspect", "asset": "/Game/A.uasset"},
                        {"action": "create", "asset": "/Game/A", "operations": [], "dry_run": "false"},
                        {"action": "apply", "asset": "/Game/A", "operations": [{"op": "x", "x": float("nan")}]},
                        {"action": "inspect", "asset": "/Game/A", "expected_revision": "oops"}]:
            with self.subTest(request=request), self.assertRaises(agent.AgentError):
                self.client.execute(request)

    def test_unreal_launch_uses_argv_and_keeps_structured_errors(self):
        captured = []
        def run(command, **kwargs):
            captured.append(command)
            request = Path(next(s.split("=", 1)[1] for s in command if s.startswith("-Request=")))
            self.assertEqual(json.loads(request.read_text())["asset"], "/Game/Players/BP_Player")
            response = Path(next(s.split("=", 1)[1] for s in command if s.startswith("-Response=")))
            response.write_text(json.dumps({"ok": False, "saved": False, "error": "Compile failed", "operation_index": 2}), encoding="utf-8")
            return subprocess.CompletedProcess(command, 1)
        with patch.object(agent, "require_editor_closed"), patch.object(agent.subprocess, "run", side_effect=run):
            result = self.client.execute({"action": "inspect", "asset": "/Game/Players/BP_Player"})
        self.assertFalse(result["ok"])
        self.assertEqual(result["operation_index"], 2)
        self.assertEqual(captured[0][1], str(self.project))
        self.assertTrue(Path(result["logs"]).is_dir())

    def test_compile_and_response_options_are_validated_before_launch(self):
        for request in [
                {"action": "compile"}, {"action": "compile", "path": "/Game/Folder/BP_One"},
                {"action": "inspect", "asset": "/Game/A", "response_mode": "summary"},
                {"action": "apply", "asset": "/Game/A", "operations": [],
                 "response_mode": "graph", "graph": "EventGraph"}]:
            agent.validate_request(request)
        for request in [
                {"action": "compile", "path": path} for path in
                [None, [], "/Engine", "/Gameplay", "/Game/../A", "/Game/A.uasset", "/Game/A/", "/Game//A"]
        ] + [
                {"action": "compile", "response_mode": "summary"},
                {"action": "inspect", "asset": "/Game/A", "response_mode": []},
                {"action": "inspect", "asset": "/Game/A", "response_mode": "brief"},
                {"action": "inspect", "asset": "/Game/A", "response_mode": "graph"},
                {"action": "inspect", "asset": "/Game/A", "response_mode": "graph", "graph": " "},
                {"action": "inspect", "asset": "/Game/A", "graph": "EventGraph"}]:
            with self.subTest(request=request), patch.object(agent, "require_editor_closed") as closed:
                with self.assertRaises(agent.AgentError):
                    self.client.execute(request)
                closed.assert_not_called()

    def test_missing_response_and_timeout_are_not_reported_as_success(self):
        for effect in [subprocess.CompletedProcess([], 7), subprocess.TimeoutExpired("Unreal", 1)]:
            with patch.object(agent, "require_editor_closed"), patch.object(agent.subprocess, "run") as run:
                if isinstance(effect, Exception):
                    run.side_effect = effect
                else:
                    run.return_value = effect
                with self.assertRaises(agent.AgentError):
                    self.client.execute({"action": "inspect", "asset": "/Game/A"})

    def test_post_save_crash_preserves_saved_state_but_returns_error(self):
        def run(command, **kwargs):
            response = Path(next(s.split("=", 1)[1] for s in command if s.startswith("-Response=")))
            response.write_text('{"ok":true,"saved":true}', encoding="utf-8")
            return subprocess.CompletedProcess(command, 5)
        with patch.object(agent, "require_editor_closed"), patch.object(agent.subprocess, "run", side_effect=run):
            result = self.client.execute({"action": "inspect", "asset": "/Game/A"})
        self.assertFalse(result["ok"])
        self.assertTrue(result["saved"])

    def test_editor_conflict_detection(self):
        for command in [f'UnrealEditor.exe "{self.project}"', "", "UnrealEditor.exe Project"]:
            with patch.object(agent, "editor_processes", return_value=[(42, command)]), self.assertRaises(agent.AgentError):
                agent.require_editor_closed(self.project)
        with patch.object(agent, "editor_processes", return_value=[(42, 'UnrealEditor.exe "D:/Other/Other.uproject"')]):
            agent.require_editor_closed(self.project)

    def test_os_lock_serializes_processes_and_releases_after_exit(self):
        lock = self.root / "lock"
        code = "import sys; from pathlib import Path; from uasset_agent import project_lock\nwith project_lock(Path(sys.argv[1]), .15): print('locked')"
        with agent.project_lock(lock):
            result = subprocess.run([sys.executable, "-c", code, str(lock)], cwd=agent.ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("lock timed out", result.stderr)
        result = subprocess.run([sys.executable, "-c", code, str(lock)], cwd=agent.ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "locked")

    def test_install_preserves_project_fields_and_makes_backup(self):
        self.project.write_text('{"FileVersion":3,"Custom":"keep","Plugins":[{"Name":"Other","Enabled":true}]}', encoding="utf-8")
        with patch.object(agent, "require_editor_closed"):
            result = agent.install_plugin(self.project)
            with self.assertRaises(agent.AgentError):
                agent.install_plugin(self.project)
        self.assertTrue((Path(result["plugin"]) / "Source/UAssetAgent/UAssetAgent.Build.cs").is_file())
        self.assertEqual((Path(result["plugin"]) / "LICENSE").read_bytes(), (agent.ROOT / "LICENSE").read_bytes())
        self.assertEqual(json.loads(self.project.read_text())["Custom"], "keep")
        self.assertEqual(len(json.loads(Path(result["project_backup"]).read_text())["Plugins"]), 1)
        self.assertEqual(len(json.loads(self.project.read_text())["Plugins"]), 2)


class McpTests(unittest.TestCase):
    def rpc(self, method, params=None):
        return agent.dispatch_rpc({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}, None)

    def test_handshake_and_discovery(self):
        self.assertEqual(self.rpc("initialize", {"protocolVersion": "2024-11-05"})["result"]["protocolVersion"], "2024-11-05")
        names = {d["name"] for d in self.rpc("tools/list")["result"]["tools"]}
        self.assertEqual(len(names), 18)
        self.assertIn("blueprint_material_inspect", names)
        self.assertIn("blueprint_material_apply", names)
        self.assertIn("blueprint_level_inspect", names)
        self.assertIn("blueprint_level_apply", names)
        self.assertIn("blueprint_references", names)
        self.assertIn("blueprint_find_path", names)
        self.assertIn("blueprint_replace_references", names)
        self.assertIn("blueprint_apply", names)
        self.assertIn("blueprint_compile", names)
        self.assertIn("blueprint_components", names)
        self.assertIn("blueprint_redirectors", names)
        self.assertIn("blueprint_delete_asset", names)
        self.assertIn("blueprint_fixup_redirectors", names)

    def test_component_and_redirector_tools_and_cli(self):
        requests = []

        class FakeClient:
            def execute(self, request):
                agent.validate_request(request)
                requests.append(request)
                return {"ok": True}

        for action, args in [("components", {"asset": "/Game/Ship"}), ("redirectors", {"path": "/Game/Example"})]:
            result = agent.dispatch_rpc({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                         "params": {"name": "blueprint_" + action, "arguments": args}}, FakeClient())["result"]
            self.assertFalse(result["isError"])
            self.assertEqual(requests[-1], dict(args, action=action))
        with patch.object(agent, "Client", return_value=FakeClient()), patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(agent.main(["--project", "dummy", "components", "/Game/Ship"]), 0)
            self.assertEqual(requests[-1], {"action": "components", "asset": "/Game/Ship"})
            self.assertEqual(agent.main(["--project", "dummy", "redirectors", "--path", "/Game/Example"]), 0)
            self.assertEqual(requests[-1], {"action": "redirectors", "path": "/Game/Example"})
        for request in [{"action": "components"}, {"action": "components", "asset": "/Game/Ship.uasset"},
                        {"action": "redirectors", "path": "/Game/../Ship"}]:
            with self.assertRaises(agent.AgentError):
                agent.validate_request(request)

    def test_compile_failures_and_response_selection_reach_mcp(self):
        requests = []
        failure = {"ok": False, "saved": False, "checked": 2, "passed": 1, "failed": 1,
                   "failures": [{"asset": "/Game/Broken", "diagnostics": [{"severity": "error", "message": "Missing function"}]}]}

        class FakeClient:
            def execute(self, request):
                agent.validate_request(request)
                requests.append(request)
                return failure if request["action"] == "compile" else {"ok": True, "revision": "a" * 40}

        for name, args in [
                ("blueprint_compile", {"path": "/Game"}),
                ("blueprint_inspect", {"asset": "/Game/A", "response_mode": "graph", "graph": "EventGraph"}),
                ("blueprint_apply", {"asset": "/Game/A", "operations": [], "expected_revision": "a" * 40, "response_mode": "summary"}),
                ("blueprint_create", {"asset": "/Game/A", "operations": [], "response_mode": "summary"})]:
            response = agent.dispatch_rpc({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                           "params": {"name": name, "arguments": args}}, FakeClient())["result"]
            self.assertEqual(requests[-1], dict(args, action=name.removeprefix("blueprint_")))
            self.assertEqual(response["isError"], name == "blueprint_compile")
            if name == "blueprint_compile":
                self.assertEqual(json.loads(response["content"][0]["text"]), failure)

    def test_cli_compile_and_graph_selection(self):
        with patch.object(agent, "Client") as client, patch("sys.stdout", new_callable=io.StringIO):
            client.return_value.execute.return_value = {"ok": True}
            self.assertEqual(agent.main(["--project", "dummy", "compile"]), 0)
            client.return_value.execute.assert_called_with({"action": "compile", "path": "/Game"})
            self.assertEqual(agent.main(["--project", "dummy", "inspect", "/Game/A", "--response-mode", "graph", "--graph", "EventGraph"]), 0)
            client.return_value.execute.assert_called_with({"action": "inspect", "asset": "/Game/A", "response_mode": "graph", "graph": "EventGraph"})
            client.return_value.execute.return_value = {"ok": False, "failed": 1}
            self.assertEqual(agent.main(["--project", "dummy", "compile", "/Game/A"]), 1)

    def test_invalid_messages_and_unknown_methods(self):
        self.assertEqual(agent.dispatch_rpc([], None)["error"]["code"], -32600)
        self.assertEqual(self.rpc("unknown")["error"]["code"], -32601)
        self.assertEqual(self.rpc("tools/call", {"name": "nope"})["error"]["code"], -32602)
        self.assertEqual(self.rpc("tools/call", {"name": "blueprint_apply", "arguments": {"asset": "/Game/A", "operations": []}})["error"]["code"], -32602)

    def test_protocol_is_available_without_launching_unreal(self):
        result = self.rpc("tools/call", {"name": "blueprint_protocol"})["result"]
        self.assertFalse(result["isError"])
        self.assertIn("add_node", result["content"][0]["text"])

    def test_execution_error_is_mcp_tool_error(self):
        class FakeClient:
            def execute(self, request):
                raise agent.AgentError("Revision conflict")
        response = agent.dispatch_rpc({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
            "name": "blueprint_inspect", "arguments": {"asset": "/Game/A"}}}, FakeClient())
        self.assertTrue(response["result"]["isError"])
        self.assertIn("Revision conflict", response["result"]["content"][0]["text"])

    def test_stream_has_one_json_response_per_request_and_no_notification_reply(self):
        source = io.StringIO('\n'.join(['{"jsonrpc":"2.0","method":"notifications/initialized"}',
                                       '{"jsonrpc":"2.0","id":1,"method":"ping"}', 'bad-json']) + '\n')
        output = io.StringIO()
        agent.serve_mcp(None, source, output)
        messages = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]["result"], {})
        self.assertEqual(messages[1]["error"]["code"], -32700)


if __name__ == "__main__":
    unittest.main()
