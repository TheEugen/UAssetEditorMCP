# UAsset Agent JSON protocol, version 18

### Layered Blend per Bone

In an animation pose graph, add the node with one or more branch-filter layers. Bones are validated when the AnimBP has a target skeleton. Each layer exposes a pose and a weight pin. Connect locomotion to `BasePose`, the emote to `BlendPoses_0`, and the node's `Pose` to the downstream pose input. Further layers are evaluated in array order.

```json
{"op":"add_node","graph":"AnimGraph","kind":"layered_blend_per_bone","id":"emote","layers":[{"weight":1,"branches":[{"bone":"clavicle_l","depth":1}]}]}
```

### Create a montage from a sequence

`asset_create` accepts a `montage` object for `/Script/Engine.AnimMontage`. Unreal creates a default section and a track covering the full source sequence. The slot defaults to `DefaultSlot` and must exist on the source skeleton. An optional `skeleton` package asserts the expected skeleton. `asset_inspect` reports the saved tracks, source paths, length and root-motion flag.

```json
{"action":"asset_create","asset":"/Game/Animations/AM_Dodge","class":"/Script/Engine.AnimMontage","montage":{"sequence":"/Game/Animations/AS_Dodge","slot":"DefaultSlot"},"dry_run":true}
```

### Animation sequences

`asset_inspect` returns `animation` with the skeleton, duration, root motion, compression settings, codec class and time/value pairs for each float curve. Use `asset_apply` with the sequence revision. `set_float_curve` replaces the named curve's keys with linear keys in seconds. `apply_distance_curve` runs Unreal's DistanceCurveModifier once on root motion using its XY defaults. Enable Animation Locomotion Library in the project. Regenerating an existing curve requires `replace_existing: true`. The optional compression check reads the running platform's compressed sample buffer.

```json
{"op":"set_float_curve","curve":"Stride","keys":[{"time":0,"value":0},{"time":0.1,"value":1}]}
{"op":"apply_distance_curve","curve":"Distance","sample_rate":30,"stop_at_end":false,"replace_existing":true}
{"op":"set_curve_compression","settings":"/Game/Characters/Heroes/Mannequin/Animations/UniformIndexableCurveCompressionSettings","verify_indexed_curve":"Distance"}
```

Targets Unreal Engine 5.4.4 normal class, Animation, Widget and Function Library Blueprints. The plugin loads the actual
asset, edits Unreal's graph objects, compiles, and saves through `SavePackage`.
Use package paths such as `/Game/Characters/BP_Player`, never filesystem paths
or `.uasset` extensions. Close this project's editor for mutations, including dry runs. Read-only queries work with it open and report `may_not_see_unsaved_editor_changes: true` because the commandlet reads the saved packages.

## Workflow

1. `list` or `inspect` to discover assets, node GUIDs, pin names and a revision.
2. `functions` to discover native or generated Blueprint functions.
3. Send `apply` with `expected_revision`, `operations`, and `dry_run: true`.
4. If compilation succeeds, send the same batch with `dry_run: false`.
5. Inspect the saved asset. Use the new revision for subsequent batches.

Each request starts a fresh commandlet process. A dry run's new node GUIDs are
temporary: refer to nodes created within the batch by aliases, not GUIDs from
a previous dry run. An `apply` request modifies one asset. Operation failure or compile
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

### Redirectors

```json
{"action":"redirectors","path":"/Game/Example"}
```

Returns `redirectors` separately from the Blueprint-only `list` action. `path`
defaults to `/Game` and selects a folder recursively. Each entry contains its
package `asset`, `object_path`, immediate `destination` (when the registry tag
is available), `chain`, and `status`:

- `resolved`: the chain ends at an on-disk asset or at its Blueprint generated
  class or class default object. `resolved_destination` retains the object path.
- `missing`: a destination is empty or its package/object is absent.
- `cycle`: a redirector appears twice in the chain.
- `unknown`: missing/malformed metadata, a native script destination, or the
  256-hop traversal limit prevented a conclusion.

This reads on-disk asset registry metadata without loading or following the
redirector UObject, so a broken target cannot hide the redirector. `resolved`
means the target exists in the registry, not that the asset can compile or load
successfully. This action does not fix references or delete redirector files.
Blueprint inspection/edit requests reject paths that redirect to a different
package; use the destination package path instead.

### Compact inspection and edit responses

`inspect`, `apply` and `create` accept `response_mode`:

- `full` (default): the existing snapshot, including variables, class defaults,
  graphs, nodes and pins.
- `summary`: asset and parent class, variable and component metadata, and graph names, object
  paths, node counts and event names/GUIDs/replication/reliability. Omits full
  nodes, pins, class defaults and component defaults. Useful for finding graphs and auditing RPCs.
- `graph`: asset and parent class plus exactly one complete graph; requires
  `graph` with an exact name or object path from the summary. Omits variables,
  class defaults and other graphs. An absent or ambiguous graph is an error;
  use an object path to disambiguate nested graphs with identical names.
- `minimal` (apply only) returns success/save status, diagnostics, created
  aliases, the revision and any backup. A dry run returns the input revision.

Set `include_components:false` in `summary` or `full` to omit the component
list. For targeted inspection, pass `node_filter` with one or more of
`class` (short node class name or full class path), `function` (call target
name), `variable` (get/set member name), and `references_path` (Unreal path
prefix found in a pin's type/default, member owner or serialized node asset).
All supplied filters must match. A filtered response returns asset identity
and matching nodes grouped by graph, including their links to other nodes.
Set `response_mode:"graph"` and `graph` to search only one graph. Each graph
reports its total and matching node counts.

```json
{"action":"inspect","asset":"/Game/Characters/BP_Player","response_mode":"summary","include_components":false}
{"action":"inspect","asset":"/Game/Characters/BP_Player","response_mode":"graph","graph":"EventGraph"}
{"action":"inspect","asset":"/Game/Ships/BP_Galleon","node_filter":{"class":"K2Node_VariableGet","references_path":"/Game/Ocean/"}}
{"action":"apply","asset":"/Game/Ships/BP_Galleon","expected_revision":"<SHA-1>","dry_run":true,"response_mode":"minimal","operations":[]}
```

Revisions, backup paths, diagnostics, node aliases and save/dry-run status
retain their usual meaning. Graph selection
is checked against the resulting Blueprint before any save, including for edits.
Use `full` to read class defaults before changing them.

### Batch compile check

```json
{"action":"compile","path":"/Game"}
{"action":"compile","path":"/Game/Characters"}
{"action":"compile","path":"/Game/Characters/BP_Player"}
```

`path` defaults to `/Game`. An existing package selects that asset; otherwise
the path selects a folder recursively. One commandlet process discovers and
compiles all matching Blueprint assets, including Blueprint subclasses such as
animation and widget Blueprints, which also support the edits described below.
Level Blueprints embedded in maps and content outside `/Game` are not scanned.

The command never saves assets or creates asset backups. Logs and derived data
can still be written. It continues after ordinary load/compile failures and
returns `checked`, `passed`, `failed`, and `failures`. Each failure contains
`asset`, `error`, and compiler `diagnostics` when available. Passing assets,
including assets with warnings only, are omitted from the response. An empty
match is an error, avoiding a false success for a mistyped path.

```json
{
  "ok":false, "saved":false, "protocol_version":2, "path":"/Game",
  "checked":2, "passed":1, "failed":1,
  "failures":[{"asset":"/Game/BP_Broken","error":"Blueprint compilation failed",
    "diagnostics":[{"severity":"error","message":"Compiler error text"}]}],
  "error":"One or more Blueprints failed to compile"
}
```

Any failure produces `ok:false`, a nonzero CLI exit code and an MCP tool error
with the structured report intact. Crashes/timeouts can prevent a complete
report; inspect the retained logs in that case. Set a larger client and MCP host
timeout for a project-wide check, for example CLI `--timeout 1800` before the
`compile` subcommand. Rebuild changed C++ first. Compilation reads saved packages.

### Function lookup and edits

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
(64-bit), `string`, `name`, `text`. Containers include `none`, `array`, `set` and `map`. For a map, the outer type describes the key and `value_type` describes the scalar value. `softobject` and `softclass` take a class `path`. `delegate` and `mcdelegate` take the object path of an existing delegate signature UFunction in `signature`.
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

```json
{"op":"delete_variable","name":"UnusedReplicatedValue"}
```

Deletes a member declared by this Blueprint, including replicated members.
Inherited/native variables, component variables, and local function variables
are not eligible. The operation scans the asset registry and uses Unreal's
variable-reference search to reject known uses in this Blueprint and hard
referencers; remove those uses first. It does not automatically delete getters,
setters or a RepNotify function. Dynamic access by name and code references are
outside this search. Compile affected Blueprints after changing public members.

### Changing a local variable's type

```json
{"op":"change_variable_type","name":"Ocean","type":{
  "category":"object","path":"/Game/Ocean/BP_Ocean_Time.BP_Ocean_Time_C"
}}
```

Use the same type description as `add_variable`. The operation edits the
declaration in place, retaining its name, GUID and compatible member flags.
Supply `default` to set a new Blueprint class default using the new type.
If a graph connection is incompatible with the new type, disconnect or remove
the old use earlier in the batch and wire the replacement after the type
change. Replicated variables cannot change to set types.

## Re-parenting

```json
{"op":"reparent","parent_class":"/Script/MyGame.NewShipBase"}
```

Accepts a native class path or generated Blueprint class path such as
`/Game/Ships/BP_Base.BP_Base_C`. Rejects self/descendant cycles, invalid parents,
local-variable collisions with inherited members, and changes between the
Actor, ActorComponent and UObject families. The current Blueprint must compile
before the batch, as with other edits.

Re-parenting refreshes nodes, repairs SCS root references, and uses Unreal's
delta-serialization compile options. Follow it with any necessary graph repairs
in the same batch; the resulting Blueprint must compile before saving. A new
parent can remove inherited functions, components or defaults from the previous
parent, so inspect the dry-run result before saving. Other assets are not saved
or migrated by this operation; run a batch compile on affected children/callers.

## Component-template defaults

```json
{"action":"components","asset":"/Game/Ships/BP_Ship"}
```

Returns `components` and the file `revision` without graph output. Each component
has a stable `id`, `name`, `class`, `source` (`local`, `inherited`, or `native`),
`inherited`, `editable`, and editable/Blueprint-visible `defaults` in Unreal text
format. Full Blueprint inspection also includes these; summaries omit defaults.

```json
{"op":"set_component_default","component":"Hull","property":"RelativeLocation","value":"(X=0,Y=0,Z=25)"}
{"op":"set_component_default","component":"native:CollisionCylinder","property":"CapsuleRadius","value":"55"}
```

Use the component `id` from inspection, or a unique component name. Local SCS
components edit their templates. Inherited Blueprint components create/update
an override owned by this Blueprint; the parent's template is unchanged. Native
components edit the generated class default object's subobject. Inherited
components must allow inherited editing. Only objects owned by the requested
Blueprint's package can be modified.

`property` names one top-level reflected property; structs and containers use a
complete Unreal text value. Values are validated in temporary storage before
assignment. Transient, edit-constant and instance-only properties are rejected.
This does not add/remove components, change attachments, edit placed actors,
or configure components created dynamically by a construction script.

### Replacing a component class

```json
{"op":"replace_component_class","component":"Solver","class":"/Game/Ocean/BPC_NewSolver.BPC_NewSolver_C"}
```

Use a component name or ID from `blueprint_components`. The operation
replaces an authored SCS component on its declaring Blueprint. It retains the
component variable name, GUID, attachment and compatible template properties.
The replacement must be a concrete Actor Component class in the same scene
or non-scene category. Inherited SCS components can be changed on their
declaring Blueprint. Native subobjects and the generated default scene root
are outside this operation.

Both type changes reconstruct affected member nodes. For edits to saved
assets, the commandlet also checks Blueprint dependents and reports them in
`affected_blueprints`. It compiles dependent Blueprints in memory and blocks
the save if a dependency fails or a child has a component template override
of the old class. The operation saves the selected Blueprint asset. Placed
actor instances and unrelated asset references require their own migration.

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

Edit a locally declared existing custom event by node GUID or batch alias:

```json
{"op":"edit_custom_event","node":"<event GUID>","replication":"none"}
{"op":"edit_custom_event","node":"<event GUID>","replication":"server","reliable":true}
{"op":"edit_custom_event","node":"<event GUID>","inputs":[{"name":"Count","type":{"category":"int"}}]}
```

Omitted fields stay unchanged. `replication: "none"` also clears reliability;
`reliable:true` without a replicated mode is rejected. `inputs` replaces the
complete ordered parameter list; `[]` removes every parameter. Names and types
use the same format as `add_node`. Unchanged parameters retain wiring and
defaults. Removing, renaming or changing the type of a linked parameter is
rejected unless it is disconnected first or `allow_break_links:true` is supplied.
That option explicitly discards affected links on the event and its local calls.

The event GUID and execution wiring are preserved, and call nodes in this
Blueprint get the updated signature. Inherited overrides and delegate-bound
events whose signatures are not editable are rejected. External caller assets
are not rewritten; use the batch compile action to detect required caller
changes. To remove the unused multicast entirely, use the existing `delete_node`
operation instead. Run the usual dry-run/apply workflow for all these operations.

### Retargeting function calls

`retarget_call` changes the function called by an ordinary K2 call node in
place. It preserves the node GUID, position and comment. Select the node by its
GUID from graph inspection or use a batch alias. Graph inspection includes the
call's `function` and `function_class`. Use the generated `_C` class path for
a Blueprint Function Library.

```json
{
  "op": "retarget_call",
  "graph": "ReadOceanTime",
  "node": "<call-node-guid>",
  "class": "/Game/Ocean/BFL_Ocean.BFL_Ocean_C",
  "function": "ReadTime",
  "pin_map": {"OldOcean": "Ocean", "Time": "Result"}
}
```

Omit `class` to keep the current class or use `"self"` for a function in the
calling Blueprint or its parents. The commandlet matches compatible pins by
name and direction. Use `pin_map` for renamed pins, with old names or pin GUIDs
as keys and new pin names as values. Linked pins and edited input defaults must
have a compatible destination. New and unused pins take the replacement
function's defaults. Missing pins, incompatible wires and invalid defaults
fail the batch without saving. Disconnect obsolete links earlier in the same
batch when the replacement signature requires it. Specialized nodes such as
interface messages and parent calls use their own node types.

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
Dry runs return the proposed snapshot in the selected mode but no new file revision. The client adds
`logs`, pointing to retained request/response/Unreal logs for that invocation.
`commandlet_result` reports the native commandlet's result, and
`process_exit_code` reports the Unreal process exit code. Unreal log errors
observed during an otherwise successful request are returned under
`engine_log_error_count` and `engine_log_warnings`. The full messages are in
the invocation log.

Reference-sensitive preflights that load packages return `reference_scan`.
Its `complete` flag and `failed_packages` list identify packages Unreal could
not check and the reason for each failure. The tool blocks the associated save
when a required package could not be checked.

Errors contain `error`, and `operation_index` (zero-based) when an individual
operation failed. Compile diagnostics include severity and message.
No validation or compilation error saves the asset. Filesystem save errors,
process crashes or timeouts have uncertain outcomes: inspect the asset and
backup before retrying. Never blindly retry a create or a partially reported
save. Successful save does not prove game behavior; test it in Unreal/PIE.

Backups live under `<project>/Saved/UAssetAgent/Backups/<id>/`. They cover the
single Blueprint `.uasset`; split packages with `.uexp`/`.ubulk` are not
supported. Source control checkout is manual; read-only assets are rejected.
The client lock serializes cooperating clients on the same project. Calling the
commandlet directly bypasses that lock and the editor-process check.

See the graph authoring operations below for actor components, variables, interfaces, macro libraries and timelines.
Inspection is a graph description, not a lossless asset round-trip format.

## Animation Blueprints (protocol 2)

Inspection returns `kind:"animation"`, `skeleton`, `template`, variables, inherited
CDO defaults and all graphs, including animation, state/transition, layer and
ordinary function graphs. Full and graph modes include node classes, titles,
pins and pose links. Anim nodes additionally contain `properties` (Unreal text)
and `functions` with `InitialUpdateFunction`, `BecomeRelevantFunction` and
`UpdateFunction` (name or null). Runtime properties use paths such as
`Node.Sequence`. Summary contains graph metadata and events, never full nodes
or pin lists. Select nested graphs by their object path when names repeat.

```json
{"action":"inspect","asset":"/Game/Ship/ABP_Wheel","response_mode":"graph","graph":"AnimGraph"}
```

All existing event-graph operations work. `delete_node` can remove overrides
such as BlueprintUpdateAnimation/BlueprintPostEvaluateAnimation, AnimNotify
events and custom events, subject to the node's normal deletion rules.

The following operations can be combined in one `apply` batch:

```json
{"op":"reparent","parent_class":"/Script/YourGame.WheelAnimInstance"}
{"op":"delete_variable","name":"WheelRotation","retarget_references":true}
{"op":"delete_function","name":"OldUpdateWheel"}
{"op":"set_anim_node_function","graph":"AnimGraph","node":"<guid>","slot":"UpdateFunction","function":"NativeUpdateWheel"}
{"op":"set_anim_node_function","graph":"AnimGraph","node":"<guid>","slot":"BecomeRelevantFunction","function":null}
{"op":"set_anim_node_property","graph":"AnimGraph","node":"<guid>","property":"Sequence","value":"/Game/Ship/AS_Wheel.AS_Wheel"}
{"op":"set_anim_node_property","graph":"AnimGraph","node":"<guid>","property":"Node.PlayRate","value":"0.5"}
{"op":"set_default","property":"RotateSound","value":"/Game/Audio/S_Wheel.S_Wheel"}
```

- Reparent accepts native UAnimInstance subclasses or generated AnimBP classes;
  it rejects other parents and inheritance cycles. Nodes refresh after the batch.
- `retarget_references:true` is opt-in for AnimBPs. It requires an inherited
  property with exactly the same name and pin type. Local getter/setter nodes
  retain their GUIDs and wiring while their local member GUID is cleared.
  If reparenting introduces that member, put `reparent` before the deletion.
  Explicitly scheduled retarget deletions are prepared before changing the
  parent, avoiding UE's duplicate-variable renaming. Other collisions fail.
  References in other assets are not rewritten. Without this flag, v1's
  reference-checking deletion behavior applies.
- Bindings accept only the three slots above. The function must exist on the
  new parent or the AnimBP, carry BlueprintThreadSafe metadata, and match UE's
  animation-update prototype (animation context and node reference). Unknown,
  unsafe or incompatible functions fail. Null clears the binding. Define any
  Blueprint function before binding it; this op does not create functions.
- Node properties must be editable and accept Unreal text. A bare runtime field
  is resolved beneath the node's runtime struct; dot paths traverse structs only.
  Whole runtime structs and function-binding fields cannot be replaced through
  this op. Setting a property does not disconnect an exposed, wired input pin;
  use the pin operations when that pin controls the value.
- `delete_function` accepts an ordinary K2 function graph name, on AnimBPs or
  Widget BPs. It cannot delete animation layers/state machines; surviving calls
  to the deleted function must be repaired in the same batch or compilation fails.
  Explicitly scheduled deletions that collide with a new parent's function are
  prepared before reparenting, preventing transient duplicate-function errors.
- `set_default` includes editable inherited native properties, including object
  references such as sounds, attenuation settings and montages. Specialized
  defaults are applied after the structural edits compile, then compiled again.
  Thus they target the final generated class, regardless of their batch position.
- Specialized batches may repair an initially uncompilable Blueprint; the
  completed batch must compile before saving. Normal BP precompile behavior is
  unchanged. New state machines/layers, Control Rig and IK Rig edits are out of scope.

Creation supports test fixtures and ordinary assets:

```json
{"action":"create","asset":"/Game/__UAssetAgentTests/ABP_Test","kind":"animation","parent_class":"/Script/Engine.AnimInstance","template":true,"operations":[],"dry_run":true,"response_mode":"summary"}
```

Use `skeleton:"/Game/.../Skeleton.Skeleton"` for a concrete AnimBP, or
`template:true` for a skeleton-independent template. Creating new anim nodes is
not exposed; the animation node ops edit existing nodes.

## Widget Blueprints (protocol 2)

Inspection returns `kind:"widget"`, `parent_class`, graphs and `widget_tree`.
Each widget has `name`, `class`, `parent` (empty for the root), zero-based `index`,
`is_variable` and, in full mode, editable `properties`. Panel slots contain
`class` and full-mode `properties`. `bindings` describes property bindings;
`animations` lists names, start/end times and lengths in seconds. Event nodes
include component-bound events with `widget` and `delegate`. Graph mode omits
the tree; summary omits widget/slot defaults and all node/pin lists.

```json
{"op":"add_widget","id":"entry4","class":"/Script/UMG.Overlay","name":"Overlay_Chainshot","parent":"AmmoEntries","index":4,"slot":{"Padding":"(Left=4,Top=0,Right=4,Bottom=0)"},"is_variable":true}
{"op":"add_widget","id":"button","class":"/Script/UMG.Button","name":"Btn_EnemyShip","parent":"entry4"}
{"op":"set_widget_property","widget":"button","property":"Visibility","value":"Visible"}
{"op":"set_widget_property","widget":"Label","property":"Text","value":"Chainshot"}
{"op":"set_slot_property","widget":"entry4","property":"Padding","value":"(Left=8,Top=0,Right=8,Bottom=0)"}
{"op":"move_widget","widget":"entry4","parent":"AmmoEntries","index":5,"slot":{"HorizontalAlignment":"HAlign_Fill"}}
{"op":"rename_widget","widget":"Label","name":"Label_Chainshot"}
{"op":"remove_widget","widget":"UnusedEntry"}
{"op":"bind_widget_event","widget":"button","event":"OnClicked","id":"clicked","graph":"EventGraph","x":0,"y":200}
{"op":"connect","from":{"node":"clicked","pin":"then"},"to":{"node":"callQuickComm","pin":"execute"}}
{"op":"reparent","parent_class":"/Script/UMG.UserWidget"}
```

- Widget selectors accept names or `add_widget` aliases from the same batch.
  Responses contain `widget_ids` mapping aliases to final names and `node_ids`
  for graph aliases, including bound event nodes.
- Omit `parent` (or use empty string) to create the root of an empty tree.
  Panel parents include CanvasPanel, Overlay, HorizontalBox, VerticalBox, GridPanel,
  ScrollBox, SizeBox and Border. Single-child panels must have room. Children can
  be native widgets or generated UserWidget classes. Recursive nesting is invalid.
- Index is zero-based; omission appends. Move uses the index in the resulting
  list; moving the root or creating a cycle is rejected. Moving between parents
  preserves editable slot values only if the slot classes match. Supply `slot`
  overrides for a different panel type. Slot values must be text strings.
- `set_widget_property`/`set_slot_property` validate editable reflected fields
  and Unreal text before assignment; dot paths traverse structs (e.g. `Font.Size`).
  Tree ownership fields and `bIsVariable` cannot be changed through property ops.
  Copy the desired editable slot values from full inspection to match an existing
  entry's layout. Add entries 4 then 5 in that order to keep their final indices.
- Remove deletes the local subtree. Rename/removal fails clearly when graph
  references, bound events, property bindings, animations, desired focus, explicit
  navigation or inherited members prevent a safe change. Remove those references
  first. The conservative property-path/navigation checks may reject unrelated
  bindings; this version does not rewrite binding/animation/navigation assets.
  Inherited widget trees and named-slot tree editing are not supported.
- `bind_widget_event` requires `is_variable:true` and a BlueprintAssignable
  multicast delegate. It finds the existing event or creates a component-bound
  event, returning its GUID under `id`; wire it using v1 `connect` and `add_node`
  call operations. An existing binding in a different graph is an error.
- V1 event-graph ops support Construct/Destruct/Tick overrides. Struct pin text,
  including `(TagName="PP.GamePhase.PostRound")`, is passed through the K2 schema
  validation used by `set_pin_default`. Tags must be registered in the project.
- Reparent accepts UUserWidget native/generated subclasses and rejects other
  families. Structural widget edits regenerate the skeleton and compile the full
  widget class before saving, preserving tree changes in the generated class.

```json
{"action":"create","asset":"/Game/__UAssetAgentTests/W_Test","kind":"widget","parent_class":"/Script/UMG.UserWidget","operations":[{"op":"add_widget","id":"root","name":"Root","class":"/Script/UMG.Overlay"}],"dry_run":true,"response_mode":"summary"}
```

## Blueprint Function Libraries

Use `kind: "function_library"` with `blueprint_create`. The parent defaults to
`/Script/Engine.BlueprintFunctionLibrary`. Library graphs can contain static
functions and local graph logic. The supported operations are `add_function`,
`edit_function`, `delete_function`, `add_node` (call, branch, sequence and reroute), `retarget_call`,
`connect`, `disconnect`, `set_pin_default`, `move_node` and `delete_node`.

```json
{
  "asset": "/Game/Utilities/BFL_Ship",
  "kind": "function_library",
  "dry_run": true,
  "response_mode": "summary",
  "operations": [
    {"op":"add_function","name":"EchoCount","pure":true,"entry_id":"entry","result_id":"result",
     "inputs":[{"name":"Count","type":{"category":"int"}}],
     "outputs":[{"name":"Value","type":{"category":"int"}}]},
    {"op":"connect","graph":"EchoCount","from":{"node":"entry","pin":"Count"},"to":{"node":"result","pin":"Value"}}
  ]
}
```

`pure` defaults to false on creation. Pure functions still use execution pins
inside their own graph, while their call nodes expose data pins only. Inspect a
saved library in `summary` mode for its revision, generated class path and
reflected function signatures, which can include Unreal's generated
`__WorldContext` parameter. Use `graph` mode for one function body. Pass
the generated class path (for example `/Game/Utilities/BFL_Ship.BFL_Ship_C`) to
`blueprint_functions` or to the `class` field of an `add_node` call operation.

```json
{"op":"edit_function","name":"EchoCount","inputs":[
  {"name":"Count","type":{"category":"int"}},
  {"name":"Extra","type":{"category":"int"}}
]}
```

`edit_function` accepts any of `inputs`, `outputs` and `pure`. Each supplied
parameter list replaces that side of the signature, and omitted fields retain
their values. Disconnect changed parameters inside the function before
replacing them. An empty list removes that side's user parameters. Optional
`entry_id` and `result_id` aliases identify the entry and a single return node
in the same batch. Use `delete_function` with the function's `name` to remove it.

An apply batch that changes an existing library function signature or deletes
a function reports `affected_callers`. It finds call nodes in Blueprint assets
and level scripts by following on-disk package references. Each affected caller
includes its package, Blueprint object path, function names, compatibility
result and compiler diagnostics. The commandlet compiles callers in memory
and saves only the library. A failed caller compile, a referenced deletion, or
a removed, retyped or purity-changing signature used by an external caller
blocks the save. An additive signature change can proceed when all affected
callers compile. Run these checks through the normal revision-guarded dry run
and repeat the same batch to save.

## Material instances (protocol 3)

`blueprint_material_inspect` takes a saved Material Instance Constant package
under `/Game`. It returns the parent object path, revision and effective scalar,
vector, texture and static-switch values, each marked if overridden locally.
Use `blueprint_material_apply` with that revision. The default is a dry run.

```json
{
  "action":"material_apply",
  "asset":"/Game/Ocean/MI_Water",
  "expected_revision":"<revision>",
  "dry_run":true,
  "operations":[
    {"op":"set_parent","parent":"/Game/Ocean/M_Water.M_Water"},
    {"op":"set_parameter","name":"Speed","type":"scalar","value":0.6},
    {"op":"set_parameter","name":"Tint","type":"vector","value":[0.1,0.2,0.4,1.0]},
    {"op":"set_parameter","name":"Normal","type":"texture","value":"/Game/Ocean/T_Normal.T_Normal"},
    {"op":"set_parameter","name":"Use Foam","type":"static_switch","value":true},
    {"op":"clear_parameter","name":"Depth","type":"scalar"}
  ]
}
```

The selector defaults to `association: "global"`. For a material layer or
blend, copy its `association` and zero-based `index` from inspection. Clearing
one parameter restores its inherited value. Parent changes validate the
inheritance chain. Apply validates the finished material using a synchronous
SM5 shader compile and reports diagnostics in `shader_errors`. A successful
save backs up the previous asset under `Saved/UAssetAgent/Backups`.

## Placed actors (protocol 4)

Use `blueprint_level_inspect` with the map's package path. The optional `search`
matches actor names, labels, class paths and GUIDs. Each actor includes its
package, transform, tags, edited instance properties and referenced assets.
The revision covers the map and its external actor files. Inspect individual
streamed sublevel assets with their own package paths.

```json
{"action":"level_inspect","asset":"/Game/Maps/Harbor","search":"Ocean"}
```

Use `blueprint_level_apply` with the returned revision. Actor selectors accept
a GUID, exact object name, or unique label. Use a GUID to resolve ambiguity.
Property values use Unreal property text and dot paths traverse struct fields.
Transforms specify location in centimeters, rotation in pitch/yaw/roll
degrees and scale as three numbers each.

```json
{
  "action":"level_apply",
  "asset":"/Game/Maps/Harbor",
  "expected_revision":"<40-character SHA-1>",
  "dry_run":true,
  "operations":[
    {"op":"replace_actor_class","actor":"BP_Ocean_Time_0","class":"/Game/Ocean/BP_Ocean_Time.BP_Ocean_Time_C"},
    {"op":"set_actor_property","actor":"BuoyancySolver","property":"BuoyancyDataset","value":"/Game/Ocean/DA_Buoyancy.DA_Buoyancy"},
    {"op":"set_actor_transform","actor":"BP_Waterline_Ocean_Gen_4","location":[100,200,1750],"rerun_construction":["BuoyancySolver"]},
    {"op":"spawn_actor","class":"/Script/Engine.TargetPoint","name":"DockTarget","transform":{"location":[100,200,300],"rotation":[0,90,0],"scale":[1,1,1]}},
    {"op":"set_tags","actor":"DockTarget","tags":["Dock","Spawn"]},
    {"op":"delete_actor","actor":"OldTarget"}
  ]
}
```

Replacement keeps the actor name, GUID, transform, tags and same-type
editable property overrides. The result identifies copied and skipped overrides.
`set_actor_transform` edits a placed actor in world space. Supply at least one
of `location`, `rotation` or `scale`. Omitted fields keep their current values.
The editor move uses the existing actor and its attachment and instance data.
It runs the actor's construction script using Unreal's completed-move behavior.
Use `rerun_construction` for dependent actors. Their packages are included in
the same save, and the operation returns their GUIDs and resulting transforms.
The tool checks loaded references for incompatible actor types and compiles
the new Blueprint class and any modified level script. Dry runs are the
default. A save backs up changed existing packages under
`Saved/UAssetAgent/Backups` and reports `changed_packages` and the new revision.
World Partition actors are saved in their external actor packages. The tool
attempts disk rollback from the backups on write failure and reports any
unrestored packages in `rollback_failures`. External actor files must be
in the map's external actor directory.

## Asset references (protocol 5)

`blueprint_references` returns the sorted package paths in `references`.
The default direction is `referencers` (incoming). Use `dependencies` for
outgoing references. Set `recursive:true` to follow the same direction through
the entire dependency graph. `include_soft` defaults to true. With
`include_soft:false`, every traversed edge must be a hard package reference.

`blueprint_find_path` searches packages recursively under `folder` (default
`/Game`) for direct references to `path_prefix`. The prefix matches complete
path segments. It can identify one package or a folder subtree. `matches`
lists each matching package and the matched target `references`. Map matches
can include external actors and report their physical package paths in
`source_packages`.

```json
{"action":"references","asset":"/Game/Ocean/BP_Ocean_Time","direction":"referencers","recursive":true,"include_soft":false}
{"action":"find_path","path_prefix":"/Game/Ocean/","folder":"/Game/Maps"}
```

Both actions use Asset Registry package metadata, including dependencies
recorded in saved maps. They can report references to missing packages when
those paths are present in the registry. Paths assembled at runtime or stored
only in arbitrary text are outside package metadata.

## Scoped reference replacement (protocol 6)

`blueprint_replace_references` rewrites hard and soft references to one
`/Game` asset in the referring packages selected by `scope`. Each entry
selects a package or folder subtree. The source and destination assets are
retained. The operation also maps generated Blueprint classes and class
defaults, retargets same-name external members and call nodes, and refreshes
and compiles the referring Blueprints. Existing graph links must remain
compatible. Incompatible or missing members abort the edit.

```json
{
  "action":"replace_references",
  "from":"/Game/Ocean60/BP_Ocean",
  "to":"/Game/Ocean62/BP_Ocean",
  "scope":["/Game/Ships/BP_Galleon","/Game/Ships/Shared"],
  "exclude_maps":true,
  "dry_run":true
}
```

The default dry run provides a report in `packages` with each package's
replacement count and its Blueprint compile diagnostics. To save, supply
the entire returned `revisions` map as `expected_revisions` and set
`dry_run:false`. Every candidate must validate and compile before the tool
backs up and saves changed packages. It reports `backups` and
`changed_packages`, and attempts rollback from the backups on a write
failure. Any unrestored files are listed in `rollback_failures`.

Maps and their external packages are excluded by default and reported in
`excluded_maps`. Set `exclude_maps:false` to include serialized asset
references in them. Use `level_apply` to change a placed actor's class.

## Asset maintenance (protocol 2)

These are independent actions, not `apply` operations. They default to
`dry_run:true` and require an explicit `dry_run:false` for disk changes.

```json
{"action":"delete_asset","asset":"/Game/Unused/BP_Test","dry_run":true,"force":false}
{"action":"delete_asset","asset":"/Game/Unused/BP_Test","expected_revision":"<40-character SHA-1>","dry_run":false,"force":false}
{"action":"fixup_redirectors","path":"/Game/Example","dry_run":true}
{"action":"fixup_redirectors","path":"/Game/Example","expected_revisions":{"/Game/Example/OldName":"<SHA-1>","/Game/Example/ReferencingBP":"<SHA-1>"},"dry_run":false}
```

Deletion returns `revision` and registry `referencers`; reference conflicts fail
with that information intact. `force:false` rejects referencers; `force:true`
explicitly permits leaving broken references and does not rewrite other assets.
An optional `expected_revision` checks stale requests. The real delete backs up
the package and returns `deleted:true`, `saved:true` and `backups`. Blueprint
deletion does not compile the deleted asset (nothing is saved), allowing cleanup
of broken assets. Registry checks cannot find names embedded in arbitrary text,
code/configuration or unregistered external files.

Fixup scans redirectors recursively, resolves chains and discovers referencers
across all registered content. It loads referencing packages (resolving hard
references), rewrites serialized soft paths, and compiles every affected Blueprint
before saving any package. Dry runs return `redirectors`, `packages_to_save` and
`revisions`; send the entire revision map back as `expected_revisions` to reject
stale or changed plans. The real run backs up **all** affected packages first,
saves referencers, rescans dependencies and deletes redirectors only when no
external referencers remain. Returns `saved_assets`, `deleted_redirectors`,
`backups`, `saved` and `save_attempted` as work proceeds.

Both actions require writable, single-asset `.uasset` packages under `/Game`;
maps, split packages and referencers outside `/Game` are rejected. Fixup fails
before saving for missing/cyclic/unknown targets or referencer compile errors.
If an outside-folder redirector refers to a selected redirector, select a common
ancestor folder. Fixup does not rewrite source code/configuration, collection
files, or arbitrary strings. A filesystem error or unresolved dependency detected
after saves can leave a **partial multi-package result**; use `saved_assets` and
`backups` to inspect/recover. No automatic rollback is promised for disk failures.

### Repairing an existing Blueprint

Set `repair: true` on `apply` to edit a Blueprint with compile errors. Inspect `initial_compile_ok` and `initial_diagnostics` for its starting state. A missing parent requires `reparent` as the first operation. The final compile, revision checks and backup requirements still apply.

### Reflected properties on non-Blueprint assets

Use `blueprint_asset_inspect`, `blueprint_asset_apply` and `blueprint_asset_create` through MCP, or send the corresponding JSON actions through the CLI. Inspection returns the class, SHA-1 revision and editable property names with their Unreal text values and C++ types. Dot paths traverse structs. Array indices are zero-based and each operation sees the result of the preceding operation.

```json
{"action":"asset_create","asset":"/Game/Labels/TestLabel","class":"/Script/Engine.PrimaryAssetLabel","properties":{"Rules.Priority":"2","bIsRuntimeLabel":"True"},"dry_run":true}
{"action":"asset_inspect","asset":"/Game/Labels/TestLabel"}
{"action":"asset_apply","asset":"/Game/Labels/TestLabel","expected_revision":"<SHA-1 from inspect>","operations":[
  {"op":"set_property","property":"Rules.Priority","value":"3"},
  {"op":"array_add","property":"ExplicitAssets","value":"/Game/Items/ItemA.ItemA"},
  {"op":"array_set","property":"ExplicitAssets","index":0,"value":"/Game/Items/ItemB.ItemB"},
  {"op":"array_remove","property":"ExplicitAssets","index":0}
],"dry_run":true}
```

Both write actions preview by default. Repeat with `dry_run:false` to persist. Updates require a single writable package and return a backup under `backups`. Creation requires a new package path and a concrete data asset class (including a generated data asset class) or a matching editor factory that can create its asset unattended. Creation accepts an optional array of operations following the initial properties. Supply values as Unreal text strings. Use the Blueprint and level tools for graph and map edits.

### Owned instanced subobjects

Use these operations with `asset_create`, `asset_apply`, or normal Blueprint `create` and `apply`. `asset_inspect` returns `subobjects`, and full Blueprint inspection returns `blueprint.default_subobjects`. Each slot reports the accepted class, whether it is an array, and its populated items. Each item reports its reflected path, class, object path, and owner. `owned: true` means the object has the slot's containing object as its Unreal outer. Owned items also expose their editable properties. Use paths such as `GEComponents[0]` or `Fragments[1].Child` as selectors. Array indices are zero-based.

| Operation | Fields and behavior |
|---|---|
| `create_subobject` | `property`, `class`, optional `owner`. Creates a concrete inline class under the selected owner. Fills an empty instanced object property or appends to an instanced object array. |
| `clone_subobject` | `property`, `source: {path, asset?}`, optional `owner`. Duplicates an owned object from the current target or from a saved `/Game` asset. A Blueprint source resolves to its class defaults. The duplicate belongs to the destination owner and includes the source object's owned children. |
| `set_subobject_property` | `target`, `property`, `value`. Updates an editable property on an owned object. `property` may traverse structs, and `value` uses Unreal text format. |

`owner` defaults to the asset or Blueprint class default object. Slots must be editable reflected instanced object fields. Blueprint batches apply these operations in order after graph changes and ordinary class defaults, then compile again. Batches with these operations preview by default. Existing packages require `expected_revision` and receive the usual backup on save. The edited asset or Blueprint defaults must pass native data validation. Its messages appear in `validation_diagnostics`.

For a Gameplay Effect Blueprint, use a registered project tag and a `GameplayEffect` parent. This example adds a five-second effect and a target tag component.

```json
{"action":"create","asset":"/Game/Abilities/GE_LegSlow","parent_class":"/Script/GameplayAbilities.GameplayEffect","operations":[
  {"op":"set_default","property":"DurationPolicy","value":"HasDuration"},
  {"op":"set_default","property":"DurationMagnitude","value":"(ScalableFloatMagnitude=(Value=5.0))"},
  {"op":"create_subobject","property":"GEComponents","class":"/Script/GameplayAbilities.TargetTagsGameplayEffectComponent"},
  {"op":"set_subobject_property","target":"GEComponents[0]","property":"InheritableGrantedTagsContainer.Added","value":"(GameplayTags=((TagName=\"PP.Status.LegSlowed\")))"}
],"dry_run":true}
```

To reuse a Lyra fragment in another item-definition Blueprint, add an operation such as `{"op":"clone_subobject","property":"Fragments","source":{"asset":"/Game/Items/ID_Source","path":"Fragments[0]"}}`. Inspect the saved destination to verify its fragment's owner and property values.

### Blueprint diff

Call `blueprint_diff` with `a` and `b`. Either may be a /Game package or an absolute .uasset file under this project's Saved/UAssetAgent/Backups directory. Optional `normalize_paths` maps folder prefixes such as `{"/Game/Older/":"/Game/Newer/"}`. The response contains `identical` and ordered changes with a path, change kind, and before/after values. Node paths contain their GUID.

### Typed async nodes and multicast delegates

Use these node kinds inside a Blueprint `create` or `apply` batch.

| `add_node` kind | Fields and behavior |
|---|---|
| `async` | `class`, `function`, optional `node_class`. Spawns the registered async Blueprint node for a static factory. Specify the node class when multiple actions match. Inspect the actual pins and wire the async outputs with `connect`. |
| `gameplay_message_listener` | `channel`, `payload_type`. Spawns the Gameplay Message Router listener in an event graph. The channel must be a registered gameplay tag and the payload must be a Blueprint-visible struct path matching the sender's payload. The node exposes `OnMessageReceived`, `ActualChannel`, the typed `Payload` and `AsyncTaskProxy` (shown as Async Action in the editor). Set `MatchType` with `set_pin_default` if you need partial matching. |
| `create_delegate` | Creates a callback reference. Connect `self` when the callback is on another object and wire `OutputDelegate` to a binding node so Unreal can determine its signature. Then send `set_delegate_function` with `node` and the name of an existing `function` or custom event. |
| `bind_delegate`, `unbind_delegate` | `delegate`, optional `class` (defaults to `self`). Bind or unbind an individual callback on a Blueprint-assignable multicast delegate. Connect the callback, execution and target pins. Unreal checks the callback signature during compilation. |

```json
{"op":"add_node","kind":"gameplay_message_listener","id":"listen",
 "channel":"PP.Player.QuickComm","payload_type":"/Game/Messages/ST_QuickComm.ST_QuickComm"}
```

Enable Gameplay Message Router in the project to use its listener. To pair a widget's `Construct` and `Destruct` events, store the listener's `AsyncTaskProxy` output in a variable of type `/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage`. Use the listener's `then` pin to set the variable, route `OnMessageReceived` to your callback, and call `/Script/Engine.CancellableAsyncAction.Cancel` on the stored proxy from `Destruct` if it is valid. Clear the variable after canceling. This lets the widget subscribe again when it is reopened. Exact pin names and types are returned by full or graph inspection.

### Blueprint graph authoring

Use these operations inside a Blueprint `create` or `apply` batch.

| Operation | Fields and behavior |
|---|---|
| `rename_variable` | `name`, `new_name`. Renames a locally declared member and its graph references while preserving its member GUID and RepNotify function. External variable references must identify the member by GUID. |
| `set_variable_replication` | `name` and at least one of `replicated`, `condition` (such as `COND_OwnerOnly`), or `rep_notify` (existing function name or empty string). |
| `add_component` | `name`, concrete component `class`, optional `parent` scene component name or ID. An unattached scene component uses the existing scene root if available. |
| `remove_component` | `component` name or ID. Removes a locally authored component, promotes its children and checks for references or inherited overrides. |
| `implement_interface` | `interface` is a native or generated interface class path. Create a Blueprint interface with `kind:"interface"` and declare methods with `add_function`. |
| `add_macro` | `name`, optional `inputs` and `outputs` using function parameter descriptors (including `{"category":"exec"}`). Use `entry_id` and `result_id` aliases to wire its tunnel nodes in the named graph. Use `kind:"macro_library"` to create a reusable library. |

Additional `add_node` kinds use the existing `graph` and `id` selectors.

```json
{"op":"add_node","kind":"cast","id":"pawn","class":"/Script/Engine.Pawn"}
{"op":"add_node","kind":"make_struct","id":"make","struct":"/Script/Engine.WalkableSlopeOverride"}
{"op":"add_node","kind":"break_struct","id":"break","struct":"/Script/Engine.WalkableSlopeOverride"}
{"op":"add_node","kind":"select","id":"choose","index_type":{"category":"bool"}}
{"op":"add_node","kind":"foreach","id":"each"}
{"op":"add_node","kind":"macro","id":"expand","macro":"Pass","library":"/Game/Macros/BPM_Utils.BPM_Utils"}
{"op":"add_node","kind":"timeline","id":"clock","name":"Progress","length":2,"loop":false,"autoplay":false,"float_tracks":[{"name":"Alpha","keys":[{"time":0,"value":0},{"time":2,"value":1}]}]}
```

Select accepts a bool, byte, int or enum index type. Connect ForEach's array pin to a typed array. A macro reference defaults to the current Blueprint. Timelines run in Actor event graphs. Inspect returns the timeline settings, float tracks and keys. For structs with native make or break functions, use a call node for that function.
