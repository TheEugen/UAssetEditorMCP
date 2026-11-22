# Install UAssetEditorMCP on Windows

This ZIP contains the editor plugin built for **Epic Games Launcher Unreal Engine 5.4.4, Win64**. Use the same editor build for your project. Source-built engines can have a different module BuildId. The server uses Python's standard library and the interpreter shipped with Unreal.

1. Extract the whole ZIP to a permanent folder. Close the project's Unreal Editor.
2. Open PowerShell **in the extracted folder**. Substitute your Launcher engine root and your project file below.

```powershell
$engine = 'C:\Example\UnrealEngine'
$project = 'D:\YourGame\YourGame.uproject'
$python = Join-Path $engine 'Engine\Binaries\ThirdParty\Python3\Win64\python.exe'
& $python .\uasset_agent.py --project $project install
```

The installer copies the compiled DLL into `Plugins\UAssetAgent`, enables the plugin and backs up the project descriptor. For an existing `Plugins\UAssetAgent` installation, add `--update` after `install`. If you previously installed the same descriptor in a differently named plugin folder, move that installation outside the project's Plugins directory before installing.

3. Connect your MCP client to the extracted server.

**Codex** — from the same PowerShell window:

```powershell
& $python .\configure_codex.py --engine $engine --project $project
```

For an existing Codex entry, update the command and arguments in its `[mcp_servers.unreal-blueprints]` table. The packaged `examples/codex-config.toml` shows the fields.

**Claude** — copy the `unreal-blueprints` entry from `examples/mcp-config.json` into your client's MCP configuration. Set `command` to the absolute path in `$python`, and replace the example script, project and engine paths with your own. Retain the `mcp` argument and the UTF-8 environment setting.

4. Restart the MCP client and call `blueprint_protocol`. Then call `blueprint_inspect` on a saved Blueprint in your project to verify the editor connection. The full tool reference is in `docs/protocol.md`.

For updates, extract the new ZIP into a fresh folder, run the installer with `--update`, and point your client at the new server. [Latest Win64 download](https://github.com/TheEugen/UAssetEditorMCP/releases/latest/download/UAssetEditorMCP-UE5.4.4-Win64.zip).
