# UAsset Agent — Blueprint editing for AI agents (UE 5.4)

A C++ editor plugin plus a dependency-free Python CLI/MCP server. Agents submit
JSON operations; Unreal edits the Blueprint graphs, compiles them and saves real
`.uasset` files. No binary format guessing or clipboard/UI automation is involved.

Implemented: inspect/list Blueprints, compact summaries and single-graph responses,
batch compile checks without saving, discover reflected functions, create normal,
Animation, Widget and Function Library Blueprints, re-parent supported types, inspect/edit component-template defaults,
add/delete typed member variables, edit custom-event parameters and RPC flags,
list redirectors and broken destination chains, add functions, add/call events and functions,
connect/disconnect pins, set defaults, delete/move nodes, add replicated variables,
RepNotify functions and server/client/multicast RPCs. Protocol 2 also edits anim
node properties/function bindings, retargets AnimBP variables to inherited members,
edits widget trees/slots/delegate events, checks Function Library signature changes
against Blueprint callers, deletes assets with referencer checks,
and fixes redirectors in folders. See [the protocol](docs/protocol.md)
for the precise supported subset.

For non-Blueprint assets, `blueprint_asset_inspect` returns editable reflected
properties and a revision. Use `blueprint_asset_apply` to update a property or
array element and `blueprint_asset_create` to create a data asset. Both preview
by default. Blueprint edits also support `repair: true` for compile errors.

The Win64 plugin has been built with UE 5.4.4. Rebuild the
plugin before using the new requests and run the relevant integration suite in
a disposable project.

## Install and build

Requires UE **5.4**, Python **3.9+**, and Unreal's C++ toolchain (on Windows,
Visual Studio 2022 with the Unreal/C++ workload and Windows SDK). No pip packages.
Close the target project's Unreal Editor before installing or editing assets. Read-only queries can run while it is open.

From this repository in PowerShell:

```powershell
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" install
```

This copies `Source` and the `.uplugin` into your project's `Plugins/UAssetAgent`
and enables it in the `.uproject`, retaining a backup of the project descriptor.
Use `install --update` after changing the plugin sources, then rebuild.

For a C++ project, build its Development Editor target (substitute its actual name):

```powershell
& "C:\Example\UnrealEngine\Engine\Build\BatchFiles\Build.bat" `
  YourGameEditor Win64 Development `
  "-Project=D:\YourGame\YourGame.uproject" -WaitMutex -NoHotReloadFromIDE
```

For a Blueprint-only project, package the plugin first using Unreal's BuildPlugin
command, then install the resulting plugin including its binaries:

```powershell
.\build-plugin.ps1 -EngineRoot "C:\Example\UnrealEngine"

py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" install `
  --source .\build\UE5.4 --update
```

Run builds from a shell permitted to write Unreal's normal build caches. The
plugin is editor-only and is not included in your multiplayer game's runtime.
The build script stages only plugin source, so Unreal cannot recursively copy
the package output into its own build. It refuses to overwrite an earlier package.

For an existing installation in `Plugins/UAssetEditor`, update that folder
directly (the generic installer above uses a different folder name):

```powershell
Copy-Item -LiteralPath 'C:\Example\UAssetAgent\Source' `
  -Destination 'D:\YourGame\Plugins\UAssetEditor' -Recurse -Force
Copy-Item -LiteralPath 'C:\Example\UAssetAgent\UAssetAgent.uplugin' `
  -Destination 'D:\YourGame\Plugins\UAssetEditor\UAssetAgent.uplugin' -Force
& 'C:\Example\UnrealEngine\Engine\Build\BatchFiles\Build.bat' `
  YourGameEditor Win64 Development '-Project=D:\YourGame\YourGame.uproject' `
  -WaitMutex -NoHotReloadFromIDE
```

Restart the MCP server after updating `uasset_agent.py`; keep it pointed at this
repository so `blueprint_protocol` serves the updated protocol text.

## Use from a coding agent or terminal

```powershell
$env:UE_ENGINE_ROOT = "C:\Example\UnrealEngine"

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
py -3.9 .\uasset_agent.py --project "D:\YourGame\YourGame.uproject" redirectors --path /Game/Example

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
reference. A client serializes requests per project. Close the editor throughout mutation batches, including previews. Each request starts Unreal; batching edits avoids
repeated startup costs. This version does not edit live inside an open editor.

## Connect an MCP-capable agent

For **Codex CLI**, use [examples/codex-config.toml](examples/codex-config.toml),
as a template. From the repository, pass your engine and project paths to the installer:

```powershell
& 'C:\Example\UnrealEngine\Engine\Binaries\ThirdParty\Python3\Win64\python.exe' `
  '.\configure_codex.py' --engine 'C:\Example\UnrealEngine' --project 'D:\YourGame\YourGame.uproject'
codex mcp list
```

The helper requires Python 3.11 (bundled with this UE installation), preserves
existing Codex settings, backs up the config and appends the server table.
Identical settings are left unchanged; a conflicting server table is reported
without overwriting it. Use `--config <path>` to select another config file, or
merge the example manually. Restart Codex and check `/mcp`, then ask it to read
`blueprint_protocol`. The config sets a 30-minute Unreal timeout and a longer
Codex tool timeout for project-wide compile checks. No separate MCP server or
pip package is needed. Queries read the saved packages. Close Unreal for edits.

Codex configuration fields follow the [official MCP configuration documentation](https://developers.openai.com/codex/mcp/).

Add [examples/mcp-config.json](examples/mcp-config.json) to your agent's MCP
configuration, replacing the project, engine and Python paths. Keep this repo
alongside the plugin; the Python server reads its bundled protocol document.

The server exposes `blueprint_protocol`, `blueprint_list`, `blueprint_compile`,
`blueprint_inspect`, `blueprint_components`, `blueprint_redirectors`,
`blueprint_functions`, `blueprint_create`, `blueprint_apply`, `blueprint_delete_asset`,
`blueprint_fixup_redirectors`, `blueprint_material_inspect`, `blueprint_material_apply`,
`blueprint_level_inspect`, `blueprint_level_apply`, `blueprint_references`,
`blueprint_find_path`, and `blueprint_replace_references`. It uses stdio;
it does not open a network listener. Set the agent's tool timeout high enough
for Unreal startup and compilation. Requests are processed sequentially, so
in-flight tool cancellation is not currently implemented.

`blueprint_compile({"path":"/Game"})` returns counts and only failing assets
with diagnostics. A path can also identify a folder or one Blueprint package.
Inspect/create/apply accept summary or single-graph responses. Inspection can
filter nodes by class, function, variable or referenced path. Apply can return
a minimal response with diagnostics, aliases and save information.
Reinstall/rebuild the native plugin and restart the MCP server when upgrading.

Mutation operations use the existing revision, dry-run, compile and backup
workflow. Use `change_variable_type` for a declared member and
`replace_component_class` for a local SCS component. Read the protocol for
type descriptors, component selectors and dependent Blueprint checks.

Suggested instruction for your coding agents:

> Use UAsset Agent to edit normal, Animation, Widget and Function Library Blueprints. Read blueprint_protocol first.
> Inspect the asset and use its revision. Construct one batch, dry-run it, inspect
> compiler diagnostics, then apply it. Use node aliases inside the batch and GUIDs
> for existing nodes. Never hex-edit .uasset files. Report gameplay behavior that
> still needs multiplayer testing.

## Validation and recovery

```powershell
py -3.9 -m unittest discover -s tests -p "test_*.py" -v
py -3.9 tests/check_cpp_headers.py --engine "C:\Example\UnrealEngine"
py -3.9 tests/check_cpp_headers.py --engine "C:\Example\UnrealEngine" --link
```

The header check uses the installed MSVC and engine headers to check the native
implementation. It substitutes the commandlet's reflected class declaration;
it **does not** validate UHT generation or runtime behavior. `--link` compiles the
substitute translation unit and resolves imports using UE's import libraries,
catching non-exported API calls. Its DLL is only a validation artifact, **not** an
installable plugin or substitute for a UBT build. Override
`--msvc`/`--sdk` if your toolchain is installed elsewhere.

After building/installing the plugin in the disposable `tests/Host` project:

```powershell
py -3.9 tests/integration_unreal.py --project tests/Host/UAssetAgentHost.uproject `
  --engine "C:\Example\UnrealEngine"
```

The integration check creates a unique folder of test assets and retains them
for inspection, including an intentionally broken child Blueprint. It checks
saved graph connections, replication flags, defaults, compact responses, batch
compile failure handling, backups, revision conflicts, dry runs, and persisted
re-parenting, variable deletion, event changes and native component defaults.

The plugin also includes native automation tests under `UAssetAgent` for
re-parenting/cycles, local/inherited/native component defaults, parameter-link
preservation and variable deletion, variable/component type changes with dependent Blueprint checks, and
valid/broken/cyclic redirector chains.
After installing the rebuilt plugin, run them in the disposable host project:

```powershell
& "C:\Example\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "$PWD\tests\Host\UAssetAgentHost.uproject" -unattended -NullRHI -nosound -nop4 `
  '-ExecCmds=Automation RunTests UAssetAgent' '-TestExit=Automation Test Queue Empty' `
  "-abslog=$PWD\tests\Host\Saved\UAssetAgentAutomation.log"
```

The v1 tests retain uniquely named fixtures under `/Game/UAssetAgentTests`.
The three `UAssetAgent.V2` cases create temporary animation/widget fixtures under
`/Game/__UAssetAgentTests`, exercise every new op, and delete their fixtures.
Run only v2 with `Automation RunTests UAssetAgent.V2` in the command above.

Run the new commandlet persistence suite after rebuilding the host plugin:

```powershell
py -3.9 tests/integration_v2.py --project tests/Host/UAssetAgentHost.uproject `
  --engine "C:\Example\UnrealEngine"
```

It runs dry runs and real saves, reloads in fresh processes, checks compact
responses, retargeting, inherited defaults, widget indices/slots/delegate wiring,
GameplayTag literal replacement, revision conflicts and backups, then deletes
its own temporary fixtures. The host includes the four fixture GameplayTags in
`tests/Host/Config/DefaultGameplayTags.ini`; register these tags if using another
disposable project for this suite.
For Function Libraries, run the persistence and caller-check suite against the
same disposable host project:

```powershell
py -3.9 tests/integration_function_library.py --project tests/Host/UAssetAgentHost.uproject `
  --engine "C:\Example\UnrealEngine"
```

Existing assets are backed up under `Saved/UAssetAgent/Backups` before saving.
Validation/compile failures do not save the asset. Crashes, timeouts, and save
failures require checking the returned logs and asset state before retrying.
Keep project source control in use: this is a new tool and the supported asset
scope is deliberately limited. Restore a backup with Unreal closed, then reopen
the editor. Runs and backups are retained until you remove them.

## Editing scope

Component defaults support local SCS templates, inherited Blueprint overrides
and native class default subobjects. Variable deletion rejects known references;
event parameter edits preserve unchanged wiring and update local callers.

Variable renaming, actor component creation/removal, creating state machines or
anim layers, editing Control Rig/IK Rig assets and authoring widget animations
remain outside this version. Referenced widget rename/removal fails clearly;
binding/animation/navigation reference rewriting is intentionally limited.

Maintenance actions default to dry runs. Multi-package redirector
fixup can leave partial disk results on save failures; consult its backups/logs.

## Design references

The native implementation uses Epic's [Blueprint creation API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FKismetEditorUtilities/CreateBlueprint?application_version=5.4)
and the locally installed UE 5.4 headers for graph/schema/compiler APIs. The
agent interface follows the MCP [stdio transport](https://modelcontextprotocol.io/specification/2024-11-05/basic/transports)
and [tools protocol](https://modelcontextprotocol.io/specification/2024-11-05/server/tools).

## License

UAsset Agent is distributed under the PolyForm Noncommercial License 1.0.0.
See [LICENSE](LICENSE) for the terms and commercial licensing contact.
