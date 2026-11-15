# UAsset Agent — Blueprint editing for AI agents (UE 5.4)

A C++ editor plugin plus a dependency-free Python CLI/MCP server. Agents submit
JSON operations; Unreal edits the Blueprint graphs, compiles them and saves real
`.uasset` files. No binary format guessing or clipboard/UI automation is involved.

Implemented: inspect/list Blueprints, compact summaries and single-graph responses,
batch compile checks without saving, discover reflected functions, create normal
class Blueprints, re-parent them, inspect/edit component-template defaults,
add/delete typed member variables, edit custom-event parameters and RPC flags,
list redirectors and broken destination chains, add functions, add/call events and functions,
connect/disconnect pins, set defaults, delete/move nodes, add replicated variables,
RepNotify functions and server/client/multicast RPCs. See [the protocol](docs/protocol.md)
for the precise supported subset.

**Status:** v1 was built and exercised end to end in Lyra in the reported
13 November session: list, inspect, function lookup, dry-run compilation,
create/save, apply/save/backup and stale-revision rejection. The v0.3 source
passes 16 Python tests and a C++ syntax/API check against installed UE 5.4 headers,
including four native automation test cases.
The full build is currently blocked in this workspace by denied writes to
UnrealBuildTool's external `MarketplaceRules.dll` cache, so its expanded runtime
integration checks have not run. Rebuild the plugin before using the new requests.

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

# Compact graph inventory and RPC flags, or just one complete graph:
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" inspect `
  /Game/Characters/BP_Player --response-mode summary
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" inspect `
  /Game/Characters/BP_Player --response-mode graph --graph EventGraph

# Compile all Blueprint assets under /Game in one process; save none:
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" --timeout 1800 compile /Game

# Inspect ship component defaults, or find broken redirectors:
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" components /Game/Ships/BP_Ship
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" redirectors --path /Game/Polynesien

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

The server exposes `blueprint_protocol`, `blueprint_list`, `blueprint_compile`,
`blueprint_inspect`, `blueprint_components`, `blueprint_redirectors`,
`blueprint_functions`, `blueprint_create`, and `blueprint_apply`. It uses stdio;
it does not open a network listener. Set the agent's tool timeout high enough
for Unreal startup and compilation. Requests are processed sequentially, so
in-flight tool cancellation is not currently implemented.

`blueprint_compile({"path":"/Game"})` returns counts and only failing assets
with diagnostics. A path can also identify a folder or one Blueprint package.
Inspect/create/apply accept `response_mode: "summary"` or
`response_mode: "graph", graph: "EventGraph"`; the default remains `full`.
Reinstall/rebuild the native plugin and restart the MCP server when upgrading.

New mutation operations use the existing `blueprint_apply` batch and its revision,
dry-run, compile and backup handling: `reparent`, `set_component_default`,
`delete_variable`, and `edit_custom_event`. Read the protocol for selectors,
parameter replacement behavior and reference/link checks.

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

The integration check creates a unique folder of test assets and retains them
for inspection, including an intentionally broken child Blueprint. It checks
saved graph connections, replication flags, defaults, compact responses, batch
compile failure handling, backups, revision conflicts, dry runs, and persisted
re-parenting, variable deletion, event changes and native component defaults.

The plugin also includes four native automation tests under `UAssetAgent` for
re-parenting/cycles, local/inherited/native component defaults, parameter-link
preservation and variable deletion, and valid/broken/cyclic redirector chains.
After installing the rebuilt plugin, run them in the disposable host project:

```powershell
& "F:\UnrealMarketplace\UE_5.4\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "$PWD\tests\Host\UAssetAgentHost.uproject" -unattended -NullRHI -nosound -nop4 `
  '-ExecCmds=Automation RunTests UAssetAgent' '-TestExit=Automation Test Queue Empty' `
  "-abslog=$PWD\tests\Host\Saved\UAssetAgentAutomation.log"
```

These tests retain uniquely named fixtures under `/Game/UAssetAgentTests`.

Existing assets are backed up under `Saved/UAssetAgent/Backups` before saving.
Validation/compile failures do not save the asset. Crashes, timeouts, and save
failures require checking the returned logs and asset state before retrying.
Keep project source control in use: this is a new tool and the supported asset
scope is deliberately limited. Restore a backup with Unreal closed, then reopen
the editor. Runs and backups are retained until you remove them.

## Editing scope

The re-parenting operation addresses the requested capability for KAN-12 and
KAN-36; project-specific migrations still need their own edit requests and checks.
Component defaults support local SCS templates, inherited Blueprint overrides
and native class default subobjects. Variable deletion rejects known references;
event parameter edits preserve unchanged wiring and update local callers.

Variable renaming, component creation/removal, animation and widget Blueprint
editing remain outside this version. The compile action can check standalone
Blueprint subclasses without adding support for editing them.

The reported Lyra test asset `/Game/Polynesien/Debug/BP_AgentSmokeTest` remains
a cleanup item in that project. This update adds no asset-delete operation.

## Design references

The native implementation uses Epic's [Blueprint creation API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FKismetEditorUtilities/CreateBlueprint?application_version=5.4)
and the locally installed UE 5.4 headers for graph/schema/compiler APIs. The
agent interface follows the MCP [stdio transport](https://modelcontextprotocol.io/specification/2024-11-05/basic/transports)
and [tools protocol](https://modelcontextprotocol.io/specification/2024-11-05/server/tools).
