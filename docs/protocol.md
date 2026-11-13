# UAsset Agent JSON protocol, version 1

Targets Unreal Engine 5.4 normal class Blueprints. The plugin loads the actual
asset, edits Unreal's graph objects, compiles, and saves through `SavePackage`.
Use package paths such as `/Game/Characters/BP_Player`, never filesystem paths
or `.uasset` extensions. Close this project's Unreal Editor before requests.

## Workflow

1. `list` or `inspect` to discover assets, node GUIDs, pin names and a revision.
2. `functions` to discover native or generated Blueprint functions.
3. Send `apply` with `expected_revision`, `operations`, and `dry_run: true`.
4. If compilation succeeds, send the same batch with `dry_run: false`.
5. Inspect the saved asset. Use the new revision for subsequent batches.

Each request starts a fresh commandlet process. A dry run's new node GUIDs are
temporary: refer to nodes created within the batch by aliases, not GUIDs from
a previous dry run. One request modifies one asset. Operation failure or compile
failure occurs before saving; edits in that process are discarded. Logs and
derived data can still be written during dry runs. A successful Blueprint
compile verifies graph validity, not gameplay or multiplayer correctness.

## Requests

```json
{"action":"list","path":"/Game/Characters"}
```

```json
{"action":"inspect","asset":"/Game/Characters/BP_Player"}
```

```json
{"action":"functions","class":"/Script/Engine.KismetSystemLibrary","search":"PrintString"}
```

For functions on a Blueprint class use its generated class object path, e.g.
`/Game/Characters/BP_Player.BP_Player_C`.

```json
{
  "action":"apply",
  "asset":"/Game/Characters/BP_Player",
  "expected_revision":"<40 lowercase hex characters from inspect>",
  "dry_run":true,
  "operations":[{"op":"set_default","property":"bReplicates","value":"True"}]
}
```

`expected_revision` compares the SHA-1 of the saved `.uasset`. It is required by
the MCP `blueprint_apply` tool and recommended for CLI requests. The commandlet
also checks that the original file did not change during the edit. These checks
do not lock out an independently running editor; keep it closed.

```json
{
  "action":"create",
  "asset":"/Game/Blueprints/BP_NewActor",
  "parent_class":"/Script/Engine.Actor",
  "operations":[],
  "dry_run":false
}
```

`create` rejects existing assets. Parent defaults to Actor. `apply` and `create`
require an operations array (maximum 1000). `dry_run` defaults to false.

## Types and values

Authoring type descriptors:

```json
{"category":"double"}
{"category":"bool"}
{"category":"int","container":"array"}
{"category":"object","path":"/Script/Engine.Actor"}
{"category":"class","path":"/Script/Engine.Actor"}
{"category":"struct","path":"/Script/CoreUObject.Vector"}
{"category":"enum","path":"/Script/Engine.ECollisionChannel"}
```

Primitive categories: `bool`, `byte`, `int`, `int64`, `float` (32-bit), `double`
(64-bit), `string`, `name`, `text`. Containers: `none`, `array`, `set`.
Maps, delegates, soft references and weak references cannot yet be authored.
Inspection reports Unreal's raw category: float/double appear as `real` with a
`subcategory` of `float`/`double`, and enums appear as `byte` with a type path.

Default values are **strings in Unreal text format**, even for numbers and
booleans: `"100.0"`, `"True"`, `"(X=1,Y=2,Z=3)"`, `"(1,2,3)"` for an int array.
Use `"\"Hello\""` for a quoted string property default. Pin string defaults
usually use the string directly, e.g. `"Hello"`. Inspect existing values to
learn the format used by the particular property. Object references use Unreal
object paths. Set input pin defaults only when the pin is unconnected.

## Variables and class defaults

```json
{
  "op":"add_variable","name":"Health","type":{"category":"double"},
  "default":"100.0","instance_editable":true,
  "replicated":true,"rep_notify":"OnRep_Health"
}
```

Names must be unique identifiers and must not shadow inherited members.
`rep_notify` creates an empty function graph with that name and enables both
replication and RepNotify; add its implementation in subsequent operations.
Replication conditions currently use Unreal's default (`COND_None`).

```json
{"op":"set_default","property":"Health","value":"80.0"}
{"op":"set_default","property":"bReplicates","value":"True"}
```

This changes class defaults, including editable inherited defaults. It does not
edit placed actors or live game instances. Added member defaults persist in the
Blueprint variable descriptor as well as the generated class default object.

## Functions

```json
{
  "op":"add_function","name":"ReadValue","entry_id":"entry","result_id":"result",
  "inputs":[{"name":"InputValue","type":{"category":"int"}}],
  "outputs":[{"name":"ResultValue","type":{"category":"int"}}]
}
```

Creates a user function with an entry, and a result when outputs are supplied.
The entry execution pin is initially connected to the result. Use
`connect` with `replace:true` to insert execution nodes. Inputs on a function
entry are **output graph pins**; outputs on a function result are **input graph
pins**. All operations inside the function need `"graph":"ReadValue"`.
`entry_id` and `result_id` are optional aliases for use within the batch.

## Nodes

Every `add_node` requires a unique batch `id`. All graph operations default to
`EventGraph`; specify `graph` for functions. `x`, `y`, and `comment` are optional.
Existing nodes are addressed by GUID from inspect. Batch aliases are resolved
to GUIDs, so recompilation does not leave stale node pointers in the client.
The existing Blueprint must compile before a mutation batch begins.

```json
{"op":"add_node","id":"print","kind":"call","class":"/Script/Engine.KismetSystemLibrary","function":"PrintString","x":400,"y":0}
{"op":"add_node","id":"get_health","kind":"get","variable":"Health"}
{"op":"add_node","id":"set_health","kind":"set","variable":"Health"}
{"op":"add_node","id":"check","kind":"branch"}
{"op":"add_node","id":"seq","kind":"sequence"}
{"op":"add_node","id":"this_actor","kind":"self"}
{"op":"add_node","id":"wire","kind":"reroute"}
```

For a call to a function on this Blueprint, use `"class":"self"` (the default).
External instance methods require a compatible `self` target pin connection.
Discover actual function names with `functions`; display labels often differ.

Inherited event override (rejects duplicates; inspect for existing events first):

```json
{"op":"add_node","id":"begin","kind":"event","event":"ReceiveBeginPlay"}
```

Custom event / RPC:

```json
{
  "op":"add_node","id":"server_event","kind":"custom_event",
  "name":"ServerRequestAction","replication":"server","reliable":true,
  "inputs":[{"name":"RequestedId","type":{"category":"int"}}]
}
```

Replication modes: `none`, `server`, `client`, `multicast`. Reliable requires a
replicated event. Events require an event graph. Actor replication, ownership,
authority checks and server-side validation are still your game's responsibility.
An RPC definition does not automatically cause the event to be called.

## Connections, defaults and layout

```json
{"op":"connect","from":{"node":"begin","pin":"then"},"to":{"node":"print","pin":"execute"}}
{"op":"disconnect","from":{"node":"begin","pin":"then"},"to":{"node":"print","pin":"execute"}}
{"op":"set_pin_default","node":"print","pin":"InString","value":"Hello from an agent"}
{"op":"move_node","node":"print","x":600,"y":200,"comment":"Log initialization"}
{"op":"delete_node","node":"print"}
```

`from` is an output and `to` an input. Pin references accept names or pin GUIDs.
Use exact machine pin names from inspection, not localized display labels.
Common execution pin names are `execute` (in) and `then` (out). A branch has
`Condition`, `then` and `else`. Function pins depend on the function signature.

Connections use the K2 schema to enforce type and graph compatibility. They
refuse to replace existing links unless `replace:true` is present. Automatic
conversion nodes are refused: create and wire the conversion explicitly.
`delete_node` respects Unreal's `CanUserDeleteNode` restrictions.

## Responses and failure handling

Responses contain `ok`, `saved`, and `protocol_version`. Mutation responses also
contain `diagnostics`, `blueprint`, `node_ids`, and `previous_revision`. Saved
requests return a new `revision` and, for existing assets, the `backup` path.
Dry runs return the proposed graph but no new file revision. The client adds
`logs`, pointing to retained request/response/Unreal logs for that invocation.

Errors contain `error`, and `operation_index` (zero-based) when an individual
operation failed. Compile diagnostics include severity and message.
No validation or compilation error saves the asset. Filesystem save errors,
process crashes or timeouts have uncertain outcomes: inspect the asset and
backup before retrying. Never blindly retry a create or a partially reported
save. Successful save does not prove game behavior; test it in Unreal/PIE.

Backups live under `<project>/Saved/UAssetAgent/Backups/<id>/`. They cover the
single normal Blueprint `.uasset`; split packages with `.uexp`/`.ubulk` are not
supported. Source control checkout is manual; read-only assets are rejected.
The client lock serializes cooperating clients on the same project. Calling the
commandlet directly bypasses that lock and the editor-process check.

This version does not author components, widgets, animations, materials, levels,
interfaces, macro libraries, arbitrary custom K2 node classes, variable renames,
or existing RPC/replication flag changes. Those require additional native handlers.
Inspection is a graph description, not a lossless asset round-trip format.
