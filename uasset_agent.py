#!/usr/bin/env python3
"""Dependency-free CLI and MCP server for the UE 5.4 UAssetAgent commandlet."""
from __future__ import annotations

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
    if not isinstance(action, str) or action not in {"list", "inspect", "functions", "apply", "create", "compile", "components", "redirectors", "delete_asset", "fixup_redirectors"}:
        raise AgentError("Unknown action; read blueprint_protocol for supported actions")
    if action in {"list", "compile", "redirectors", "fixup_redirectors"}:
        path = request.get("path", "/Game")
        if (not isinstance(path, str) or not (path == "/Game" or path.startswith("/Game/")) or
                any(c in path for c in (".", "\\", ":")) or
                any(not part for part in path.split("/")[1:])):
            raise AgentError("path must be a package or folder path under /Game without an extension")
    if "response_mode" in request or "graph" in request:
        if action not in {"inspect", "apply", "create"}:
            raise AgentError("response_mode and graph require inspect, apply or create")
        mode = request.get("response_mode", "full")
        if not isinstance(mode, str) or mode not in {"full", "summary", "graph"}:
            raise AgentError("response_mode must be full, summary or graph")
        if mode == "graph":
            if not isinstance(request.get("graph"), str) or not request["graph"].strip():
                raise AgentError("response_mode graph requires a nonempty graph name or object path")
        elif "graph" in request:
            raise AgentError("graph requires response_mode graph")
    if action in {"inspect", "components", "apply", "create", "delete_asset"}:
        asset = request.get("asset", "")
        if (not isinstance(asset, str) or not asset.startswith("/Game/") or
                any(c in asset for c in (".", "\\", ":")) or
                any(not part for part in asset.split("/")[1:])):
            raise AgentError("asset must be a /Game/package/path without a file extension")
    if action == "functions" and not isinstance(request.get("class"), str):
        raise AgentError("functions requires a class object path")
    if action in {"apply", "create"}:
        ops = request.get("operations")
        if not isinstance(ops, list) or len(ops) > 1000:
            raise AgentError("operations must be an array of at most 1000 operations")
        if any(not isinstance(op, dict) or not isinstance(op.get("op"), str) for op in ops):
            raise AgentError("Every operation must be an object with an op string")
    for field in ("dry_run", "force", "template"):
        if field in request and not isinstance(request[field], bool):
            raise AgentError(field + " must be a boolean")
    if action == "create":
        kind = request.get("kind", "normal")
        if not isinstance(kind, str) or kind not in {"normal", "animation", "widget"}:
            raise AgentError("kind must be normal, animation or widget")
        if kind == "animation" and not request.get("template", False) and not isinstance(request.get("skeleton"), str):
            raise AgentError("Animation creation requires skeleton or template:true")
    if "expected_revisions" in request:
        revisions = request["expected_revisions"]
        if action != "fixup_redirectors" or not isinstance(revisions, dict):
            raise AgentError("expected_revisions must be a fixup_redirectors revision map")
        for asset, revision in revisions.items():
            validate_request({"action": "inspect", "asset": asset, "expected_revision": revision})
    if "expected_revision" in request:
        revision = request["expected_revision"]
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
            if result.returncode != 0 and response["ok"]:
                response.update(ok=False, error=f"Unreal exited with code {result.returncode} after writing a response; inspect saved state before retrying")
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
        backup = project.with_name(project.name + "." + uuid.uuid4().hex[:8] + ".bak")
        shutil.copy2(project, backup)
        temporary = project.with_name(project.name + ".uasset-agent.tmp")
        temporary.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, project)
    return {"ok": True, "plugin": str(destination), "project_backup": str(backup),
            "next": "Build the project's Development Editor target for UE 5.4 before using the tool."}


def tool_definitions():
    asset = {"type": "string", "description": "Package path such as /Game/Blueprints/BP_Player"}
    response_options = {
        "response_mode": {"type": "string", "enum": ["full", "summary", "graph"], "default": "full",
                          "description": "summary omits nodes, pins and class defaults; graph returns one graph. full preserves the complete snapshot."},
        "graph": {"type": "string", "description": "Required with response_mode graph: exact graph name or object path from a summary."}}
    definitions = []
    specs = [
        ("blueprint_list", "List Blueprint assets under /Game.", {"path": {"type": "string"}}, []),
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
        ("blueprint_compile", "Compile one Blueprint or recursively compile a folder in one Unreal process, without saving. Returns counts and only failures with diagnostics. Includes Blueprint subclasses. Editor must be closed; allow a long timeout for /Game.",
         {"path": {"type": "string", "default": "/Game", "description": "Blueprint package or recursive folder path under /Game; defaults to the entire /Game tree."}}, []),
        ("blueprint_inspect", "Inspect normal, Animation or Widget Blueprints: graphs, pose links, anim function bindings/properties, widget tree/slots/bindings/animations, defaults and revision. summary omits nodes and pins; graph selects one graph.", {"asset": asset}, ["asset"]),
        ("blueprint_functions", "Discover callable functions and parameter types on a class, e.g. /Script/Engine.KismetSystemLibrary.",
         {"class": {"type": "string"}, "search": {"type": "string"}}, ["class"]),
        ("blueprint_apply", "Edit normal, Animation or Widget Blueprints, compile and save. Includes reparent, inherited variable retargeting, anim properties/function bindings, widget tree/slot edits and delegate events. Read blueprint_protocol for operation shapes and limits. Use expected_revision; dry_run compiles without saving. Editor must be closed.",
         {"asset": asset, "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
          "expected_revision": {"type": "string", "pattern": "^[0-9a-f]{40}$"}, "dry_run": {"type": "boolean"}}, ["asset", "operations", "expected_revision"]),
        ("blueprint_create", "Create a normal, animation or widget Blueprint with operations, compile and save. Animation requires skeleton or template:true and an AnimInstance parent; widget requires a UserWidget parent. Never overwrites assets.",
         {"asset": asset, "parent_class": {"type": "string"}, "operations": {"type": "array", "items": {"type": "object"}, "maxItems": 1000},
          "kind": {"type": "string", "enum": ["normal", "animation", "widget"], "default": "normal"},
          "skeleton": {"type": "string"}, "template": {"type": "boolean", "default": False},
          "dry_run": {"type": "boolean"}}, ["asset", "operations"]),
        ("blueprint_protocol", "Read the complete operation format, node types, pin references and examples before constructing an edit batch.", {}, [])]
    for name, description, properties, required in specs:
        if name in {"blueprint_inspect", "blueprint_apply", "blueprint_create"}:
            properties.update(response_options)
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
                    if not isinstance(request, dict) or request.get("action") not in {"apply", "create", "delete_asset", "fixup_redirectors"}:
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
