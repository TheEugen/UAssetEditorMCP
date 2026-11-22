"""Build, extract and exercise the Win64 buyer bundle with the specified UE editor."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ZIP_NAME = "UAssetEditorMCP-UE5.4.4-Win64.zip"
DLL = "Binaries/Win64/UnrealEditor-UAssetAgent.dll"
MODULES = "Binaries/Win64/UnrealEditor.modules"
PLUGIN = "UAssetAgent.uplugin"
CLIENT_FILES = ("uasset_agent.py", "blueprint_diff.py", "configure_codex.py", "docs/protocol.md",
                "examples/codex-config.toml", "examples/mcp-config.json", "INSTALL.md", "LICENSE")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def engine_build_id(engine, local_smoke):
    version = read_json(engine / "Engine/Build/Build.version")
    if tuple(version[k] for k in ("MajorVersion", "MinorVersion", "PatchVersion")) != (5, 4, 4):
        raise ValueError("The Win64 bundle requires Unreal Engine 5.4.4")
    build_id = read_json(engine / "Engine" / MODULES)["BuildId"]
    if not build_id:
        raise ValueError("Missing UE editor BuildId")
    if not local_smoke:
        # Check the actual registered installation; never rewrite the module manifest.
        inventory = Path(os.environ["ProgramData"]) / "Epic/UnrealEngineLauncher/LauncherInstalled.dat"
        installs = read_json(inventory)["InstallationList"]
        registered = any(entry.get("AppName") == "UE_5.4" and
                         Path(entry["InstallLocation"]).resolve() == engine for entry in installs)
        if not (registered and version["Changelist"] > 0 and (engine / "Engine/Build/InstalledBuild.txt").is_file()):
            raise ValueError("Buyer releases require the Epic Launcher installation of UE 5.4.4")
    return build_id


def smoke(extracted, project_dir, engine, build_id):
    descriptor = read_json(extracted / PLUGIN)
    manifest = read_json(extracted / MODULES)
    if not (descriptor.get("Installed") and any(m["Name"] == "UAssetAgent" and m["Type"] == "Editor"
                                                for m in descriptor["Modules"])):
        raise ValueError("Missing packaged UAssetAgent editor module")
    if manifest["BuildId"] != build_id or manifest["Modules"]["UAssetAgent"] != Path(DLL).name:
        raise ValueError("Plugin module metadata does not match the test editor")
    for name in CLIENT_FILES:
        if (extracted / name).read_bytes() != (ROOT / name).read_bytes():
            raise ValueError(f"Incorrect archived file: {name}")
    project_dir.mkdir()
    project = project_dir / "ReleaseSmoke.uproject"
    fixture = ROOT / "tests/Host"
    descriptor = read_json(fixture / "UAssetAgentHost.uproject")
    descriptor["Plugins"] = [p for p in descriptor["Plugins"] if p["Name"] != "UAssetAgent"]
    project.write_text(json.dumps(descriptor), encoding="utf-8")
    shutil.copytree(fixture / "Config", project_dir / "Config")
    command = [sys.executable, "-E", "-s", "-B", "-X", "utf8", str(extracted / "uasset_agent.py"),
               "--project", str(project), "--engine", str(engine)]
    subprocess.run(command + ["install"], cwd=project_dir, check=True)
    installed_dll = project_dir / "Plugins/UAssetAgent" / DLL
    if installed_dll.read_bytes() != (extracted / DLL).read_bytes():
        raise ValueError("The buyer installer did not install the archived DLL")
    asset = "/Game/Smoke/BP_ReleaseSmoke"
    requests = [{"id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05"}},
                {"method": "notifications/initialized"},
                {"id": 2, "method": "tools/call", "params": {"name": "blueprint_protocol", "arguments": {}}},
                {"id": 3, "method": "tools/call", "params": {"name": "blueprint_create", "arguments": {
                    "asset": asset, "operations": [], "dry_run": False}}},
                {"id": 4, "method": "tools/call", "params": {"name": "blueprint_inspect", "arguments": {
                    "asset": asset, "response_mode": "summary"}}}]
    wire = "".join(json.dumps(dict(r, jsonrpc="2.0")) + "\n" for r in requests)
    result = subprocess.run(command + ["mcp"], cwd=project_dir, input=wire, stdout=subprocess.PIPE,
                            encoding="utf-8", check=True, timeout=720)
    replies = {r["id"]: r["result"] for r in map(json.loads, result.stdout.splitlines())}
    for number in (2, 3, 4):
        if replies[number]["isError"]:
            for log in sorted(project_dir.glob("Saved/UAssetAgent/Runs/*/unreal.log")):
                errors = [line for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                          if "Error:" in line or "failed to load" in line.lower()]
                print("\n".join(errors[-15:]), file=sys.stderr)
            raise ValueError(replies[number]["content"])
    protocol = (ROOT / "docs/protocol.md").read_text(encoding="utf-8")
    if replies[2]["content"][0]["text"] != protocol:
        raise ValueError("blueprint_protocol returned different documentation")
    created, inspected = (json.loads(replies[i]["content"][0]["text"]) for i in (3, 4))
    if not (created["saved"] and inspected["blueprint"]["asset"] == asset and
            inspected["revision"] == created["revision"]):
        raise ValueError("Native inspection did not match the saved fixture")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Directory for the validated ZIP")
    parser.add_argument("--local-smoke", action="store_true", help="Allow a source engine for local testing only")
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    build_id = engine_build_id(engine, args.local_smoke)
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output, prefix="verify-") as tmp:
        work = Path(tmp).resolve()
        plugin = work / "plugin"
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(ROOT / "build-plugin.ps1"), "-EngineRoot", str(engine),
                        "-OutputDirectory", str(plugin)], cwd=ROOT, check=True)
        archive = work / ZIP_NAME
        with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as bundle:
            for name in (PLUGIN, MODULES, DLL):
                bundle.write(plugin / name, name)
            for folder in ("Source", "Config"):
                for path in sorted((plugin / folder).rglob("*")):
                    if path.is_file():
                        bundle.write(path, path.relative_to(plugin))
            for name in CLIENT_FILES:
                bundle.write(ROOT / name, name)
        extracted = work / "extracted"
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(extracted)
        smoke(extracted, work / "project", engine, build_id)
        name = "local-smoke-" + ZIP_NAME if args.local_smoke else ZIP_NAME
        os.replace(archive, args.output / name)
        print(f"Validated {args.output / name} (editor BuildId {build_id})")


if __name__ == "__main__":
    main()
