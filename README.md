# UAsset Agent — Blueprint editing for AI agents (UE 5.4)

A C++ editor plugin plus a dependency-free Python CLI/MCP server. Agents submit
JSON operations; Unreal edits the Blueprint graphs, compiles them and saves real
`.uasset` files. No binary format guessing or clipboard/UI automation is involved.

Implemented: inspect/list Blueprints, discover reflected functions, create normal
class Blueprints, add typed variables and functions, add/call events and functions,
connect/disconnect pins, set defaults, delete/move nodes, add replicated variables,
RepNotify functions and server/client/multicast RPCs. See [the protocol](docs/protocol.md)
for the precise supported subset.

**Status:** all 12 Python tests passed, and the native implementation passed a
C++ syntax/API check against the installed UE 5.4 headers.
A full UnrealBuildTool build and runtime integration test are still required.
The build attempt in this workspace was blocked by denied access to Unreal's
external rules cache; no successful plugin binary is included.

## Install and build

Requires UE **5.4**, Python **3.9+**, and Unreal's C++ toolchain (on Windows,
Visual Studio 2022 with the Unreal/C++ workload and Windows SDK). No pip packages.
Close the target project's Unreal Editor before installing or running requests.

From this repository in PowerShell:

```powershell
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" install
```

This copies `Source` and the `.uplugin` into your project's `Plugins/UAssetAgent`
and enables it in the `.uproject`, retaining a backup of the project descriptor.
Use `install --update` after changing the plugin sources, then rebuild.

For a C++ project, build its Development Editor target (substitute its actual name):

```powershell
& "F:\UnrealMarketplace\UE_5.4\Engine\Build\BatchFiles\Build.bat" `
  YourGameEditor Win64 Development `
  "-Project=D:\YourGame\YourGame.uproject" -WaitMutex -NoHotReloadFromIDE
```

For a Blueprint-only project, package the plugin first using Unreal's BuildPlugin
command, then install the resulting plugin including its binaries:

```powershell
.\build-plugin.ps1 -EngineRoot "F:\UnrealMarketplace\UE_5.4"

py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" install `
  --source .\build\UE5.4 --update
```

Run builds from a shell permitted to write Unreal's normal build caches. The
plugin is editor-only and is not included in your multiplayer game's runtime.
The build script stages only plugin source, so Unreal cannot recursively copy
the package output into its own build. It refuses to overwrite an earlier package.

## Use from a coding agent or terminal

```powershell
$env:UE_ENGINE_ROOT = "F:\UnrealMarketplace\UE_5.4"

py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" list

py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" inspect `
  /Game/Characters/BP_Player

# Validate and compile an example without saving an asset:
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" run `
  .\examples\create_replicated_actor.json --dry-run

# Create it after the dry run succeeds:
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" run `
  .\examples\create_replicated_actor.json
```

The example creates `/Game/AgentExamples/BP_AgentHealth`, enables Actor replication,
adds a replicated Health variable with an empty RepNotify function, and wires a
reliable server event to reset Health. It is a graph-authoring example, not a
complete gameplay system. Test ownership, authority and gameplay validation in
your multiplayer project.

Every command emits JSON and exits nonzero on failure. `--engine` accepts an
engine root or the exact `UnrealEditor-Cmd.exe` path; Windows can also discover
the registered UE 5.4 installation. `--timeout` defaults to 300 seconds. Put
global options before the subcommand.

For existing assets, use `inspect`'s revision as `expected_revision` in your edit
request. Read [docs/protocol.md](docs/protocol.md) for examples and the operation
reference. A client serializes requests per project, but the editor must remain
closed throughout the batch. Each request starts Unreal; batching edits avoids
repeated startup costs. This version does not edit live inside an open editor.

## Connect an MCP-capable agent

Add [examples/mcp-config.json](examples/mcp-config.json) to your agent's MCP
configuration, replacing the project, engine and Python paths. Keep this repo
alongside the plugin; the Python server reads its bundled protocol document.

The server exposes `blueprint_protocol`, `blueprint_list`, `blueprint_inspect`,
`blueprint_functions`, `blueprint_create`, and `blueprint_apply`. It uses stdio;
it does not open a network listener. Set the agent's tool timeout high enough
for Unreal startup and compilation. Requests are processed sequentially, so
in-flight tool cancellation is not currently implemented.

Suggested instruction for your coding agents:

> Use UAsset Agent to edit normal class Blueprints. Read blueprint_protocol first.
> Inspect the asset and use its revision. Construct one batch, dry-run it, inspect
> compiler diagnostics, then apply it. Use node aliases inside the batch and GUIDs
> for existing nodes. Never hex-edit .uasset files. Report gameplay behavior that
> still needs multiplayer testing.

## Validation and recovery

```powershell
py -3.9 -m unittest discover -s tests -p "test_*.py" -v
py -3.9 tests/check_cpp_headers.py --engine "F:\UnrealMarketplace\UE_5.4"
```

The header check uses the installed MSVC and engine headers to check the native
implementation. It substitutes the commandlet's reflected class declaration;
it **does not** validate UHT generation, linking, or runtime behavior. Override
`--msvc`/`--sdk` if your toolchain is installed elsewhere.

After building/installing the plugin in the disposable `tests/Host` project:

```powershell
py -3.9 tests/integration_unreal.py --project tests/Host/UAssetAgentHost.uproject `
  --engine "F:\UnrealMarketplace\UE_5.4"
```

The integration check creates uniquely named test assets and retains them for
inspection. It checks saved graph connections, replication flags, defaults,
compile failure handling, backups, revision conflicts and dry runs.

Existing assets are backed up under `Saved/UAssetAgent/Backups` before saving.
Validation/compile failures do not save the asset. Crashes, timeouts, and save
failures require checking the returned logs and asset state before retrying.
Keep project source control in use: this is a new tool and the supported asset
scope is deliberately limited. Restore a backup with Unreal closed, then reopen
the editor. Runs and backups are retained until you remove them.

## Design references

The native implementation uses Epic's [Blueprint creation API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FKismetEditorUtilities/CreateBlueprint?application_version=5.4)
and the locally installed UE 5.4 headers for graph/schema/compiler APIs. The
agent interface follows the MCP [stdio transport](https://modelcontextprotocol.io/specification/2024-11-05/basic/transports)
and [tools protocol](https://modelcontextprotocol.io/specification/2024-11-05/server/tools).
