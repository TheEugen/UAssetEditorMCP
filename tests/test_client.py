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
        self.assertEqual(json.loads(self.project.read_text())["Custom"], "keep")
        self.assertEqual(len(json.loads(Path(result["project_backup"]).read_text())["Plugins"]), 1)
        self.assertEqual(len(json.loads(self.project.read_text())["Plugins"]), 2)


class McpTests(unittest.TestCase):
    def rpc(self, method, params=None):
        return agent.dispatch_rpc({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}, None)

    def test_handshake_and_discovery(self):
        self.assertEqual(self.rpc("initialize", {"protocolVersion": "2024-11-05"})["result"]["protocolVersion"], "2024-11-05")
        names = {d["name"] for d in self.rpc("tools/list")["result"]["tools"]}
        self.assertEqual(len(names), 6)
        self.assertIn("blueprint_apply", names)

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
