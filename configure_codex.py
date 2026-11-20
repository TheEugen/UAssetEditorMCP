"""Install the bundled Codex MCP configuration without replacing other settings.

Run with UE 5.4's bundled Python 3.11 (no pip dependencies).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
import tomllib
import uuid

ROOT = Path(__file__).resolve().parent
SERVER = "unreal-blueprints"


def install(config, snippet):
    config = Path(config).expanduser().resolve()
    original = config.read_bytes() if config.exists() else None
    existing_text = original.decode("utf-8-sig") if original is not None else ""
    existing = tomllib.loads(existing_text)
    desired = tomllib.loads(snippet)["mcp_servers"][SERVER]
    current = existing.get("mcp_servers", {}).get(SERVER)
    if current is not None:
        if current == desired:
            return None, False
        raise ValueError(f"{SERVER} already has different settings in {config}. Update that table using examples/codex-config.toml; no changes made.")
    combined = existing_text + "\n\n" + snippet.rstrip() + "\n"
    tomllib.loads(combined)  # Reject invalid/duplicate tables before any write.
    config.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=config.parent, prefix=".uasset-codex-", suffix=".tmp", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(combined)
        # Don't overwrite a configuration that changed while preparing the file.
        latest = config.read_bytes() if config.exists() else None
        if latest != original:
            raise ValueError("Codex configuration changed during setup; rerun the command.")
        if original is not None:
            backup = config.with_name(config.name + ".uasset-agent." + uuid.uuid4().hex + ".bak")
            shutil.copy2(config, backup)
        os.replace(temporary, config)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return backup, True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, help="Destination config.toml; defaults to Codex's user config")
    parser.add_argument("--engine", required=True, type=Path, help="Unreal Engine checkout or installation root")
    parser.add_argument("--project", required=True, type=Path, help="Target .uproject file")
    args = parser.parse_args()
    config = args.config or Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))) / "config.toml"
    try:
        engine = args.engine.resolve(strict=True)
        project = args.project.resolve(strict=True)
        python = (engine / "Engine/Binaries/ThirdParty/Python3/Win64/python.exe").resolve(strict=True)
        if not project.is_file() or project.suffix.lower() != ".uproject":
            raise ValueError("--project must identify a .uproject file")
        snippet = (ROOT / "examples/codex-config.toml").read_text(encoding="utf-8")
        for placeholder, path in {
            "C:/Example/UnrealEngine/Engine/Binaries/ThirdParty/Python3/Win64/python.exe": python,
            "C:/Example/UAssetAgent/uasset_agent.py": ROOT / "uasset_agent.py",
            "D:/YourGame/YourGame.uproject": project,
            "C:/Example/UnrealEngine": engine,
        }.items():
            snippet = snippet.replace(json.dumps(placeholder), json.dumps(path.as_posix(), ensure_ascii=False))
        backup, changed = install(config, snippet)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Setup failed: {exc}\n")
    print(("Configured " if changed else "Already configured: ") + str(config))
    if backup:
        print("Backup: " + str(backup))
    print("Restart Codex, then check `codex mcp list` or `/mcp` in the terminal UI.")


if __name__ == "__main__":
    main()
