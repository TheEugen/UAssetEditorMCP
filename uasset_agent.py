#!/usr/bin/env python3
"""Dependency-free CLI and MCP server for the UE 5.4 UAssetAgent commandlet."""
from __future__ import annotations

from blueprint_diff import compare_blueprints

import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

VERSION = "2.0.0"
MAX_REQUEST_BYTES = 4 * 1024 * 1024
ROOT = Path(__file__).resolve().parent
DISK_QUERIES = frozenset({"list", "inspect", "components", "functions", "redirectors", "references",
                          "find_path", "diff", "asset_inspect", "material_inspect", "level_inspect", "compile"})


class AgentError(Exception):
    pass


def read_json(text):
    def reject(value):
        raise ValueError("Non-finite JSON number: " + value)
    return json.loads(text, parse_constant=reject)


def validate_request(request):
    if not isinstance(request, dict):
        raise AgentError("Request must be a JSON object")
    action = request.get("action")
    if "repair" in request and (action != "apply" or not isinstance(request["repair"], bool)):
        raise AgentError("repair must be a boolean on apply")
    if not isinstance(action, str) or action not in {"diff", "list", "inspect", "functions", "apply", "create", "compile", "components", "redirectors", "delete_asset", "fixup_redirectors", "asset_inspect", "asset_apply", "asset_create", "material_inspect", "material_apply", "level_inspect", "level_apply", "references", "find_path", "replace_references"}:
        raise AgentError("Unknown action; read blueprint_protocol for supported actions")
    if action == "diff":
        for key in ("a", "b"):
            source = request.get(key)
            if not isinstance(source, str) or not (source.startswith("/Game/") or source.lower().endswith(".uasset")):
                raise AgentError("diff requires a and b as /Game packages or MCP backup files")
        redirects = request.get("normalize_paths", {})
        if not isinstance(redirects, dict) or any(not isinstance(path, str) or not path.startswith("/") or not path.endswith("/") for pair in redirects.items() for path in pair):
            raise AgentError("normalize_paths maps slash-terminated folder prefixes")
    if action in {"list", "compile", "redirectors", "fixup_redirectors"}:
        path = request.get("path", "/Game")
        if (not isinstance(path, str) or not (path == "/Game" or path.startswith("/Game/")) or
                any(c in path for c in (".", "\\", ":")) or
                any(not part for part in path.split("/")[1:])):
            raise AgentError("path must be a package or folder path under /Game without an extension")
    mode = request.get("response_mode", "full")
    if "response_mode" in request or "graph" in request:
        if action not in {"inspect", "apply", "create"}:
            raise AgentError("response_mode and graph require inspect, apply or create")
        modes = ("full", "summary", "graph", "minimal") if action == "apply" else ("full", "summary", "graph")
        if not isinstance(mode, str) or mode not in modes:
            raise AgentError("response_mode must be full, summary, graph or minimal (apply only)")
        if mode == "graph":
            if not isinstance(request.get("graph"), str) or not request["graph"].strip():
                raise AgentError("response_mode graph requires a nonempty graph name or object path")
        elif "graph" in request:
            raise AgentError("graph requires response_mode graph")
    if "node_filter" in request:
        filters = request["node_filter"]
        if action != "inspect" or mode == "summary":
            raise AgentError("node_filter requires full or graph inspection")
        if not isinstance(filters, dict) or not filters or any(
                key not in {"class", "function", "variable", "references_path"} or
                not isinstance(value, str) or not value.strip() for key, value in filters.items()):
            raise AgentError("node_filter requires nonempty class, function, variable or references_path strings")
        path = filters.get("references_path")
        if path is not None and (not path.strip().startswith("/") or len(path.strip()) <= 1):
            raise AgentError("references_path requires an Unreal path")
    if "include_components" in request and (
            action not in {"inspect", "apply", "create"} or mode not in ("summary", "full") or
            not isinstance(request["include_components"], bool)):
        raise AgentError("include_components must be a boolean and requires summary or full")
    if action in {"inspect", "components", "apply", "create", "delete_asset", "asset_inspect", "asset_apply", "asset_create", "material_inspect", "material_apply", "level_inspect", "level_apply"}:
        asset = request.get("asset", "")
        if (not isinstance(asset, str) or not asset.startswith("/Game/") or
                any(c in asset for c in (".", "\\", ":")) or
                any(not part for part in asset.split("/")[1:])):
            raise AgentError("asset must be a /Game/package/path without a file extension")
    if action == "functions" and not isinstance(request.get("class"), str):
        raise AgentError("functions requires a class object path")
    if action in {"references", "find_path"}:
        paths = {"asset": request.get("asset")} if action == "references" else {
            "path_prefix": request.get("path_prefix"), "folder": request.get("folder", "/Game")}
        for field, value in paths.items():
            if not isinstance(value, str):
                raise AgentError(field + " must be an Unreal package path")
            path = value[:-1] if action == "find_path" and value.endswith("/") else value
            if (not path.startswith("/") or any(c in path for c in (".", "\\", ":")) or
                    any(not part for part in path.split("/")[1:]) or
                    (action == "references" and "/" not in path[1:])):
                raise AgentError(field + " must be an Unreal package path")
        if action == "references" and request.get("direction", "referencers") not in ("referencers", "dependencies"):
            raise AgentError("direction must be referencers or dependencies")
        for field in ("include_soft", "recursive"):
            if field in request and not isinstance(request[field], bool):
                raise AgentError(field + " must be a boolean")
    if action == "asset_create":
        if not isinstance(request.get("class"), str) or not request["class"].strip():
            raise AgentError("asset_create requires a concrete asset class path")
        properties = request.get("properties", {})
        if not isinstance(properties, dict) or any(not key or not isinstance(value, str) for key, value in properties.items()):
            raise AgentError("properties must map property paths to Unreal text strings")
    if action in {"apply", "create", "material_apply", "level_apply", "asset_apply"} or (action == "asset_create" and "operations" in request):
        ops = request.get("operations")
        if not isinstance(ops, list) or len(ops) > 1000:
            raise AgentError("operations must be an array of at most 1000 operations")
        if any(not isinstance(op, dict) or not isinstance(op.get("op"), str) for op in ops):
            raise AgentError("Every operation must be an object with an op string")
    if action == "replace_references":
        for field in ("from", "to"):
            validate_request({"action": "inspect", "asset": request.get(field)})
        scope = request.get("scope")
        if not isinstance(scope, list) or not 1 <= len(scope) <= 1000:
            raise AgentError("scope must contain 1 to 1000 /Game package or folder paths")
        for path in scope:
            validate_request({"action": "list", "path": path.removesuffix("/") if isinstance(path, str) else path})
        if request.get("dry_run") is False and "expected_revisions" not in request:
            raise AgentError("A save requires expected_revisions from a dry run")
    for field in ("dry_run", "force", "template", "exclude_maps"):
        if field in request and not isinstance(request[field], bool):
            raise AgentError(field + " must be a boolean")
    if action == "create":
        kind = request.get("kind", "normal")
        if not isinstance(kind, str) or kind not in {"normal", "animation", "widget", "function_library", "interface", "macro_library"}:
            raise AgentError("Unknown Blueprint kind")
        if kind == "function_library":
            if request.get("parent_class", "/Script/Engine.BlueprintFunctionLibrary") != "/Script/Engine.BlueprintFunctionLibrary":
                raise AgentError("Function libraries must use /Script/Engine.BlueprintFunctionLibrary")
            if "skeleton" in request or "template" in request:
                raise AgentError("skeleton and template are animation creation options")
        if kind == "animation" and not request.get("template", False) and not isinstance(request.get("skeleton"), str):
            raise AgentError("Animation creation requires skeleton or template:true")
    if "expected_revisions" in request:
        revisions = request["expected_revisions"]
        if action not in {"fixup_redirectors", "replace_references"} or not isinstance(revisions, dict):
            raise AgentError("expected_revisions must be a fixup_redirectors or replace_references revision map")
        for asset, revision in revisions.items():
            validate_request({"action": "inspect", "asset": asset, "expected_revision": revision})
    if action in {"material_apply", "level_apply", "asset_apply"} or "expected_revision" in request:
        revision = request.get("expected_revision")
        if not isinstance(revision, str) or len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision):
            raise AgentError("expected_revision must be the SHA-1 revision returned by inspect")
    try:
        encoded = json.dumps(request, ensure_ascii=False, allow_nan=False).encode("utf-8")
    except (ValueError, TypeError) as exc:
        raise AgentError(str(exc)) from exc
    if len(encoded) > MAX_REQUEST_BYTES:
        raise AgentError("Request exceeds 4 MiB")


@contextmanager
def project_lock(path, timeout=60):
    """OS-owned lock: released on crash; never delete the lock file (inode races)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as handle:
        handle.seek(0, os.SEEK_END)
        if handle.tell() == 0:
            handle.write(b"\0")
            handle.flush()
        deadline = time.monotonic() + timeout
        while True:
            try:
                handle.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as exc:
                if time.monotonic() >= deadline:
                    raise AgentError("Another agent is using this project; lock timed out") from exc
                time.sleep(0.1)
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def editor_processes():
    """Read process command lines without interpolating user data into a shell."""
    if os.name == "nt":
        script = ("$ErrorActionPreference='Stop'; "
                  "@(Get-CimInstance Win32_Process -Filter \"Name = 'UnrealEditor.exe'\" | "
                  "Select-Object ProcessId,CommandLine) | ConvertTo-Json -Compress")
        completed = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, timeout=20, check=True,
            creationflags=subprocess.CREATE_NO_WINDOW)
        result = read_json(completed.stdout.strip() or "[]")
        if isinstance(result, dict):
            result = [result]
        return [(p["ProcessId"], p.get("CommandLine") or "") for p in result]
    completed = subprocess.run(["ps", "-ax", "-o", "pid=,args="], capture_output=True,
                               text=True, check=True, timeout=20)
    result = []
    for line in completed.stdout.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) == 2 and "UnrealEditor" in fields[1] and "-run=" not in fields[1]:
            result.append((int(fields[0]), fields[1]))
    return result


def require_editor_closed(project):
    try:
        processes = editor_processes()
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
        raise AgentError("Cannot verify whether Unreal Editor is closed: " + str(exc)) from exc
    for pid, command in processes:
        normalized = command.replace("\\", "/").casefold()
        # Bare project names are accepted by Unreal, too. Missing/ambiguous command
        # lines cannot establish that the process belongs to a different project.
        if (not normalized or project.name.casefold() in normalized or
                ".uproject" not in normalized):
            raise AgentError(f"Close Unreal Editor for this project before running edits (PID {pid})")


def find_editor(engine):
    if not engine:
        engine = os.environ.get("UE_ENGINE_ROOT")
    if not engine and os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\EpicGames\Unreal Engine\5.4") as key:
                engine = winreg.QueryValueEx(key, "InstalledDirectory")[0]
        except OSError:
            pass
    if not engine:
        raise AgentError("Set --engine or UE_ENGINE_ROOT to your Unreal Engine 5.4 installation")
    root = Path(engine).expanduser().resolve()
    if root.is_file():
        return root
    candidates = [root / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
                  root / "Engine/Binaries/Linux/UnrealEditor-Cmd",
                  root / "Engine/Binaries/Linux/UnrealEditor",
                  root / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise AgentError(f"UnrealEditor commandlet executable not found in {root}")


class Client:
    def __init__(self, project, engine=None, timeout=300, lock_timeout=60):
        self.project = Path(project).expanduser().resolve()
        if self.project.suffix.lower() != ".uproject" or not self.project.is_file():
            raise AgentError("--project must point to an existing .uproject file")
        self.editor = find_editor(engine)
        if not math.isfinite(timeout) or timeout <= 0 or not math.isfinite(lock_timeout) or lock_timeout < 0:
            raise AgentError("Timeouts must be finite; process timeout > 0 and lock timeout >= 0")
        self.timeout, self.lock_timeout = timeout, lock_timeout
        self.saved = self.project.parent / "Saved/UAssetAgent"

    def execute(self, request):
        validate_request(request)
        with project_lock(self.saved / "project.lock", self.lock_timeout):
            disk_query = request["action"] in DISK_QUERIES
            if not disk_query:
                require_editor_closed(self.project)
            run_dir = self.saved / "Runs" / uuid.uuid4().hex
            run_dir.mkdir(parents=True)
            request_path = run_dir / "request.json"
            response_path = run_dir / "response.json"
            log_path = run_dir / "unreal.log"
            request_path.write_text(json.dumps(request, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")
            command = [str(self.editor), str(self.project), "-run=UAssetAgent",
                       f"-Request={request_path}", f"-Response={response_path}",
                       "-unattended", "-nop4", "-NullRHI", "-nosound", "-nosplash",
                       "-UTF8Output", f"-abslog={log_path}"]
            try:
                with (run_dir / "process.log").open("wb") as output:
                    result = subprocess.run(command, cwd=self.project.parent, stdout=output,
                                            stderr=subprocess.STDOUT, timeout=self.timeout,
                                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            except subprocess.TimeoutExpired as exc:
                raise AgentError(f"Unreal timed out. Inspect the asset and logs before retrying; save state is unknown. Logs: {run_dir}") from exc
            except OSError as exc:
                raise AgentError(f"Cannot launch Unreal: {exc}. Logs: {run_dir}") from exc
            if not response_path.is_file():
                raise AgentError(f"Unreal exited with code {result.returncode} without a response. Build/enable UAssetAgent and inspect {run_dir}")
            try:
                response = read_json(response_path.read_text(encoding="utf-8-sig"))
            except (ValueError, UnicodeError) as exc:
                raise AgentError(f"Invalid commandlet response. Logs: {run_dir}") from exc
            if not isinstance(response, dict) or not isinstance(response.get("ok"), bool):
                raise AgentError(f"Malformed commandlet response. Logs: {run_dir}")
            response["logs"] = str(run_dir)
            if disk_query:
                response["may_not_see_unsaved_editor_changes"] = True
            if result.returncode != 0 and response["ok"]:
                response.update(ok=False, error=f"Unreal exited with code {result.returncode} after writing a response; inspect saved state before retrying")
            if request["action"] == "diff" and response["ok"]:
                response["changes"] = compare_blueprints(response.pop("a"), response.pop("b"), request.get("normalize_paths", {}))
                response["identical"] = not response["changes"]
            return response


def install_plugin(project, source=ROOT, update=False):
    project = Path(project).expanduser().resolve()
    source = Path(source).resolve()
    if not project.is_file() or project.suffix.lower() != ".uproject":
        raise AgentError("--project must be an existing .uproject")
    if not (source / "UAssetAgent.uplugin").is_file():
        raise AgentError("Plugin source does not contain UAssetAgent.uplugin")
    destination = project.parent / "Plugins/UAssetAgent"
    if destination.resolve() == source:
        raise AgentError("Source and destination plugin directories must differ")
    with project_lock(project.parent / "Saved/UAssetAgent/project.lock"):
        require_editor_closed(project)
        if destination.exists() and not update:
            raise AgentError(f"Plugin already exists at {destination}; use --update to replace its files")
        descriptor = read_json(project.read_text(encoding="utf-8-sig"))
        if not isinstance(descriptor, dict):
            raise AgentError("Project descriptor must be a JSON object")
        plugins = descriptor.setdefault("Plugins", [])
        if not isinstance(plugins, list) or any(not isinstance(p, dict) or not isinstance(p.get("Name"), str) for p in plugins):
            raise AgentError("Invalid Plugins field in .uproject")
        existing = next((p for p in plugins if p.get("Name") == "UAssetAgent"), None)
        if existing is None:
            plugins.append({"Name": "UAssetAgent", "Enabled": True})
        else:
            existing["Enabled"] = True
        destination.mkdir(parents=True, exist_ok=True)
        for name in ("Source", "Binaries", "Config", "Resources"):
            if (source / name).is_dir():
                shutil.copytree(source / name, destination / name, dirs_exist_ok=True)
        shutil.copy2(source / "UAssetAgent.uplugin", destination / "UAssetAgent.uplugin")
        if (source / "LICENSE").is_file():
            shutil.copy2(source / "LICENSE", destination / "LICENSE")
        backup = project.with_name(project.name + "." + uuid.uuid4().hex[:8] + ".bak")
        shutil.copy2(project, backup)
        temporary = project.with_name(project.name + ".uasset-agent.tmp")
        temporary.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, project)
    return {"ok": True, "plugin": str(destination), "project_backup": str(backup),
            "next": ("Start the MCP server with this project's UE 5.4.4 editor."
                     if (source / "Binaries/Win64/UnrealEditor-UAssetAgent.dll").is_file()
                     else "Build the project's Development Editor target for UE 5.4 before using the tool.")}


def tool_definitions():
    asset = {"type": "string", "description": "Package path such as /Game/Blueprints/BP_Player"}
    response_options = {
        "response_mode": {"type": "string", "enum": ["full", "summary", "graph"], "default": "full",
                          "description": "summary omits nodes, pins and class defaults; graph returns one graph. full preserves the complete snapshot."},
        "graph": {"type": "string", "description": "Required with response_mode graph: exact graph name or object path from a summary."},
        "include_components": {"type": "boolean", "default": True, "description": "Set false to omit components from full or summary."}}
    definitions = []
    specs = [
        ("blueprint_list", "List Blueprint assets under /Game.", {"path": {"type": "string"}}, []),
        ("blueprint_diff", "Compare two Blueprint packages or MCP backups, matching nodes by GUID. Includes variables, class defaults, components, pins and links.",
         {"a": {"type": "string"}, "b": {"type": "string"}, "normalize_paths": {"type": "object", "additionalProperties": {"type": "string"}}}, ["a", "b"]),
        ("blueprint_asset_inspect", "Inspect a saved non-Blueprint asset's class, editable properties in Unreal text format, and revision.",
         {"asset": asset}, ["asset"]),
        ("blueprint_asset_apply", "Edit reflected properties and array elements on a non-Blueprint asset. Defaults to a dry run.",
         {"asset": asset, "expected_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"},
          "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
          "dry_run": {"type": "boolean", "default": True}}, ["asset", "expected_revision", "operations"]),
        ("blueprint_asset_create", "Create a concrete data asset or an asset with a compatible headless factory. Defaults to a dry run.",
         {"asset": asset, "class": {"type": "string"}, "properties": {"type": "object", "additionalProperties": {"type": "string"}},
          "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
          "dry_run": {"type": "boolean", "default": True}}, ["asset", "class"]),
        ("blueprint_material_inspect", "Inspect a Material Instance Constant, its parent and its scalar, vector, texture and static switch parameters.", {"asset": asset}, ["asset"]),
        ("blueprint_material_apply", "Apply Material Instance Constant parameter overrides or change its parent. Defaults to a dry run. Read blueprint_protocol for operations.", {
            "asset": asset, "expected_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"},
            "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
            "dry_run": {"type": "boolean", "default": True}}, ["asset", "expected_revision", "operations"]),
        ("blueprint_level_inspect", "Inspect placed actors in a map's persistent level, including World Partition actors. Returns GUIDs, transforms, tags, instance property overrides, referenced assets and a revision.", {
            "asset": asset, "search": {"type": "string", "description": "Match actor name, label, class path or GUID."}}, ["asset"]),
        ("blueprint_level_apply", "Replace, spawn, delete or edit placed actors, including World Partition actors. Defaults to a dry run. Use the revision from level inspection and read blueprint_protocol for operations.", {
            "asset": asset, "expected_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"},
            "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
            "dry_run": {"type": "boolean", "default": True}}, ["asset", "expected_revision", "operations"]),
        ("blueprint_references", "Read package dependents or dependencies from Unreal's Asset Registry. Can traverse recursively and include soft references.", {
            "asset": asset, "direction": {"type": "string", "enum": ["referencers", "dependencies"], "default": "referencers"},
            "recursive": {"type": "boolean", "default": False}, "include_soft": {"type": "boolean", "default": True}}, ["asset"]),
        ("blueprint_find_path", "Find packages under a folder with a reference into a package path prefix. Includes map imports and World Partition actors.", {
            "path_prefix": {"type": "string", "description": "Target package or folder, e.g. /Game/Ocean."},
            "folder": {"type": "string", "default": "/Game", "description": "Source folder to search recursively."},
            "include_soft": {"type": "boolean", "default": True}}, ["path_prefix"]),
        ("blueprint_replace_references", "Replace references to one asset within selected packages or folders. Dry run previews edits and Blueprint compile diagnostics. Saves require the preview's revisions.", {
            "from": asset, "to": asset, "scope": {"type": "array", "items": asset, "minItems": 1, "maxItems": 1000},
            "exclude_maps": {"type": "boolean", "default": True}, "dry_run": {"type": "boolean", "default": True},
            "expected_revisions": {"type": "object", "additionalProperties": {"type": "string", "pattern": "^[0-9a-f]{40}$"}}},
         ["from", "to", "scope"]),
        ("blueprint_redirectors", "List on-disk redirectors recursively, including missing destinations. Reports destination, chain and resolved/missing/cycle/unknown status from asset registry metadata; does not fix or delete assets.",
         {"path": {"type": "string", "default": "/Game"}}, []),
        ("blueprint_delete_asset", "Delete a single /Game asset with backup and registry referencer check. Defaults to dry run and force:false. force:true can leave broken references; read blueprint_protocol. Use revision from inspection or the deletion dry run.",
         {"asset": asset, "expected_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"},
          "dry_run": {"type": "boolean", "default": True}, "force": {"type": "boolean", "default": False}}, ["asset"]),
        ("blueprint_fixup_redirectors", "Fix redirectors recursively in a /Game folder. Defaults to dry run; returns revisions for expected_revisions on save. Backs up affected packages and compiles referencing Blueprints before saving. Broken destinations abort. Multi-package disk failures can leave partial results; read blueprint_protocol.",
         {"path": {"type": "string", "default": "/Game"}, "dry_run": {"type": "boolean", "default": True},
          "expected_revisions": {"type": "object", "additionalProperties": {"type": "string", "pattern": "^[0-9a-f]{40}$"}}}, []),
        ("blueprint_components", "Inspect local, inherited Blueprint and native component template defaults without graph output. Returns component selectors and file revision for set_component_default operations.",
         {"asset": asset}, ["asset"]),
        ("blueprint_compile", "Compile one Blueprint or recursively compile a folder in one Unreal process, without saving. Returns counts and only failures with diagnostics. Includes Blueprint subclasses. Reads the saved packages; allow a long timeout for /Game.",
         {"path": {"type": "string", "default": "/Game", "description": "Blueprint package or recursive folder path under /Game; defaults to the entire /Game tree."}}, []),
        ("blueprint_inspect", "Inspect normal, Animation, Widget, Interface, Macro Library or Function Library Blueprints: graphs, pose links, anim function bindings/properties, widget tree/slots/bindings/animations, defaults and revision. summary omits nodes and pins; graph selects one graph.", {"asset": asset}, ["asset"]),
        ("blueprint_functions", "Discover callable functions and parameter types on a class, e.g. /Script/Engine.KismetSystemLibrary.",
         {"class": {"type": "string"}, "search": {"type": "string"}}, ["class"]),
        ("blueprint_apply", "Edit and compile a Blueprint. Supports reparenting, inherited variable retargeting, animation properties and bindings, widget trees and delegates, and static library functions with caller checks. Use retarget_call to change an existing call node while preserving compatible wiring, change_variable_type for local members, replace_component_class for authored components, rename_variable, set_variable_replication, add_component and remove_component. Read blueprint_protocol for operation shapes and limits. Use expected_revision; dry_run compiles without saving. Editor must be closed.",
         {"asset": asset, "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
          "expected_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"}, "dry_run": {"type": "boolean"}}, ["asset", "operations", "expected_revision"]),
        ("blueprint_create", "Create a normal, animation, widget, interface, macro library or function library Blueprint with operations, compile and save. Function libraries default to the BlueprintFunctionLibrary parent. Animation requires skeleton or template:true and an AnimInstance parent; widget requires a UserWidget parent. Never overwrites assets.",
         {"asset": asset, "parent_class": {"type": "string"}, "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
          "kind": {"type": "string", "enum": ["normal", "animation", "widget", "function_library", "interface", "macro_library"], "default": "normal"},
          "skeleton": {"type": "string"}, "template": {"type": "boolean", "default": False},
          "dry_run": {"type": "boolean"}}, ["asset", "operations"]),
        ("blueprint_protocol", "Read the complete operation format, node types, pin references and examples before constructing an edit batch.", {}, [])]
    for name, description, properties, required in specs:
        if name in {"blueprint_inspect", "blueprint_apply", "blueprint_create"}:
            properties.update(response_options)
        if name == "blueprint_inspect":
            properties["node_filter"] = {
                "type": "object", "properties": {field: {"type": "string", "minLength": 1}
                    for field in ("class", "function", "variable", "references_path")},
                "minProperties": 1, "additionalProperties": False,
                "description": "Return matching nodes only. Match exact node class, function, variable, or a referenced Unreal path. Multiple fields are ANDed."}
        if name == "blueprint_apply":
            properties["repair"] = {"type": "boolean", "description": "Allow a broken starting Blueprint and return initial diagnostics. Saving requires a successful final compile."}
            properties["response_mode"] = {
                "type": "string", "enum": ["full", "summary", "graph", "minimal"], "default": "full",
                "description": "minimal returns diagnostics, aliases, revision and backup/save status without an asset snapshot."}
        definitions.append({"name": name, "description": description,
                            "inputSchema": {"type": "object", "properties": properties,
                                            "required": required, "additionalProperties": False}})
    return definitions


def dispatch_rpc(message, client):
    """MCP newline-delimited JSON-RPC transport; no stdout logging."""
    if not isinstance(message, dict) or message.get("jsonrpc") != "2.0" or not isinstance(message.get("method"), str):
        return {"jsonrpc": "2.0", "id": message.get("id") if isinstance(message, dict) else None,
                "error": {"code": -32600, "message": "Invalid JSON-RPC request"}}
    if "id" not in message:
        return None
    reply = {"jsonrpc": "2.0", "id": message["id"]}
    method = message["method"]
    params = message.get("params", {})
    if not isinstance(params, dict):
        return dict(reply, error={"code": -32602, "message": "params must be an object"})
    if method == "initialize":
        requested = params.get("protocolVersion")
        protocol = requested if requested in {"2024-11-05", "2025-03-26", "2025-06-18"} else "2024-11-05"
        result = {"protocolVersion": protocol, "capabilities": {"tools": {}},
                  "serverInfo": {"name": "uasset-agent", "version": VERSION},
                  "instructions": "UE 5.4 Blueprint editor. Read blueprint_protocol, inspect, dry-run, then apply with expected_revision. Close the project editor first."}
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": tool_definitions()}
    elif method == "tools/call":
        try:
            name = params.get("name")
            definition = next((d for d in tool_definitions() if d["name"] == name), None)
            if definition is None:
                return dict(reply, error={"code": -32602, "message": "Unknown tool"})
            args = params.get("arguments", {})
            schema = definition["inputSchema"]
            if not isinstance(args, dict) or any(k not in schema["properties"] for k in args) or any(k not in args for k in schema["required"]):
                return dict(reply, error={"code": -32602, "message": "Missing or unknown tool arguments"})
            if name == "blueprint_protocol":
                result = {"content": [{"type": "text", "text": (ROOT / "docs/protocol.md").read_text(encoding="utf-8")}], "isError": False}
            else:
                response = client.execute(dict(args, action=name.removeprefix("blueprint_")))
                result = {"content": [{"type": "text", "text": json.dumps(response, ensure_ascii=False)}], "isError": not response["ok"]}
        except (AgentError, OSError, ValueError) as exc:
            result = {"content": [{"type": "text", "text": str(exc)}], "isError": True}
    else:
        return dict(reply, error={"code": -32601, "message": "Method not found"})
    return dict(reply, result=result)


def serve_mcp(client, source=None, destination=None):
    source = source or sys.stdin
    destination = destination or sys.stdout
    while True:
        line = source.readline(MAX_REQUEST_BYTES + 1)
        if not line:
            return
        if len(line.encode("utf-8")) > MAX_REQUEST_BYTES:
            # Framing has been lost; terminate instead of interpreting fragments.
            raise AgentError("MCP message exceeds 4 MiB")
        try:
            reply = dispatch_rpc(read_json(line), client)
        except (ValueError, RecursionError):
            reply = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}}
        if reply is not None:
            destination.write(json.dumps(reply, ensure_ascii=False, allow_nan=False) + "\n")
            destination.flush()


def main(argv=None):
    # MCP requires UTF-8, including when Windows redirects stdin/stdout to pipes.
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, help="Path to your .uproject")
    parser.add_argument("--engine", help="UE 5.4 root or UnrealEditor-Cmd executable")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--lock-timeout", type=float, default=60)
    sub = parser.add_subparsers(dest="command", required=True)
    install = sub.add_parser("install", help="Copy and enable the editor plugin")
    install.add_argument("--source", type=Path, default=ROOT)
    install.add_argument("--update", action="store_true")
    listing = sub.add_parser("list", help="List Blueprint assets")
    listing.add_argument("--path", default="/Game")
    redirectors = sub.add_parser("redirectors", help="List redirectors and destination status without loading them")
    redirectors.add_argument("--path", default="/Game")
    components = sub.add_parser("components", help="Inspect component templates and defaults without graphs")
    components.add_argument("asset")
    compile_command = sub.add_parser("compile", help="Compile a Blueprint or folder without saving; return only failures")
    compile_command.add_argument("path", nargs="?", default="/Game")
    inspect = sub.add_parser("inspect", help="Export a Blueprint as JSON")
    inspect.add_argument("asset")
    inspect.add_argument("--response-mode", choices=("full", "summary", "graph"), default="full")
    inspect.add_argument("--graph", help="Graph name or object path; requires --response-mode graph")
    functions = sub.add_parser("functions", help="Discover Blueprint callable functions")
    functions.add_argument("class_path")
    functions.add_argument("--search", default="")
    run = sub.add_parser("run", help="Execute a JSON request file; use - for stdin")
    run.add_argument("request")
    run.add_argument("--dry-run", action="store_true")
    sub.add_parser("mcp", help="Serve MCP over stdin/stdout")
    args = parser.parse_args(argv)
    try:
        if args.command == "install":
            result = install_plugin(args.project, args.source, args.update)
        else:
            client = Client(args.project, args.engine, args.timeout, args.lock_timeout)
            if args.command == "mcp":
                serve_mcp(client)
                return 0
            if args.command == "run":
                text = sys.stdin.read(MAX_REQUEST_BYTES + 1) if args.request == "-" else Path(args.request).read_text(encoding="utf-8-sig")
                if len(text.encode("utf-8")) > MAX_REQUEST_BYTES:
                    raise AgentError("Request exceeds 4 MiB")
                request = read_json(text)
                if args.dry_run:
                    if not isinstance(request, dict) or request.get("action") not in {"apply", "create", "delete_asset", "fixup_redirectors", "material_apply", "level_apply", "asset_apply", "asset_create", "replace_references"}:
                        raise AgentError("--dry-run requires an edit or maintenance request")
                    request["dry_run"] = True
            elif args.command == "functions":
                request = {"action": "functions", "class": args.class_path, "search": args.search}
            elif args.command == "inspect":
                request = {"action": "inspect", "asset": args.asset, "response_mode": args.response_mode}
                if args.graph is not None:
                    request["graph"] = args.graph
            elif args.command == "components":
                request = {"action": "components", "asset": args.asset}
            else:
                request = {"action": args.command, "path": args.path}
            result = client.execute(request)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0 if result.get("ok") else 1
    except (AgentError, OSError, ValueError, RecursionError) as exc:
        error = json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False)
        print(error, file=sys.stderr if args.command == "mcp" else sys.stdout)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
