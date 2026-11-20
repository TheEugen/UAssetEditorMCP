import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
HAS_TOMLLIB = importlib.util.find_spec("tomllib") is not None
if HAS_TOMLLIB:
    import tomllib
    import configure_codex


@unittest.skipUnless(HAS_TOMLLIB, "Codex setup helper requires Python 3.11; the MCP server still supports 3.9")
class CodexSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.config = Path(self.temporary.name) / "config.toml"
        self.snippet = (configure_codex.ROOT / "examples/codex-config.toml").read_text(encoding="utf-8")

    def test_preserves_existing_config_and_backs_up_exact_bytes(self):
        original = b'# Preserve my settings\r\nmodel = "example"\r\n[mcp_servers.other]\r\ncommand = "other.exe"\r\n'
        self.config.write_bytes(original)
        backup, changed = configure_codex.install(self.config, self.snippet)
        self.assertTrue(changed)
        self.assertEqual(backup.read_bytes(), original)
        parsed = tomllib.loads(self.config.read_text(encoding="utf-8"))
        self.assertEqual(parsed["model"], "example")
        self.assertEqual(parsed["mcp_servers"]["other"]["command"], "other.exe")
        before = self.config.read_bytes()
        self.assertEqual(configure_codex.install(self.config, self.snippet), (None, False))
        self.assertEqual(self.config.read_bytes(), before)

    def test_conflicts_and_invalid_toml_leave_config_untouched(self):
        for original in ('[mcp_servers.unreal-blueprints]\ncommand="keep.exe"\n', 'invalid = ['):
            self.config.write_text(original, encoding="utf-8")
            with self.assertRaises(ValueError):
                configure_codex.install(self.config, self.snippet)
            self.assertEqual(self.config.read_text(encoding="utf-8"), original)

    def test_new_config_and_timeout_headroom(self):
        backup, changed = configure_codex.install(self.config, self.snippet)
        self.assertIsNone(backup)
        self.assertTrue(changed)
        server = tomllib.loads(self.config.read_text(encoding="utf-8"))["mcp_servers"]["unreal-blueprints"]
        timeout = float(server["args"][server["args"].index("--timeout") + 1])
        self.assertGreater(server["tool_timeout_sec"], timeout + 60)
