"""Optional Windows API/syntax check using installed UE headers (not a UBT build).

UHT and linking still require the normal Unreal build. All temporary outputs stay
inside this repository. The check substitutes only our reflected class declaration.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--msvc", type=Path, default=Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130"))
    parser.add_argument("--sdk", type=Path, default=Path(r"C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / "build/header-check"
    output.mkdir(parents=True, exist_ok=True)
    engine = args.engine / "Engine"
    definitions = engine / "Intermediate/Build/Win64/x64/UnrealEditor/Development/BlueprintGraph/Definitions.BlueprintGraph.h"
    source = root / "Source/UAssetAgent/Private/UAssetAgentCommandlet.cpp"
    declaration = '''#include "Commandlets/Commandlet.h"
class UUAssetAgentCommandlet : public UCommandlet {
public: UUAssetAgentCommandlet(); virtual int32 Main(const FString& Params) override;
};'''
    translation = output / "Check.cpp"
    translation.write_text(source.read_text(encoding="utf-8").replace('#include "UAssetAgentCommandlet.h"', declaration), encoding="utf-8")
    includes = [args.msvc / "include", args.sdk / "ucrt", args.sdk / "shared", args.sdk / "um", args.sdk / "winrt",
                engine / "Source", engine / "Source/Runtime", engine / "Source/Runtime/Core/Public/Windows"]
    for folder in (engine / "Source/Runtime", engine / "Source/Editor"):
        for name in ("Public", "Classes"):
            includes.extend(folder.rglob(name))
    includes.extend((engine / "Intermediate/Build/Win64/UnrealEditor/Inc").glob("*/UHT"))
    options = ["/nologo", "/Zs", "/std:c++20", "/EHsc", "/bigobj", "/utf-8", "/permissive-", "/Zc:__cplusplus",
               '/FI"' + str(definitions) + '"']
    options.extend('/I"' + str(path) + '"' for path in includes)
    options.append('"' + str(translation) + '"')
    response = output / "compile.rsp"
    response.write_text("\n".join(options), encoding="utf-8")
    return subprocess.run([str(args.msvc / "bin/Hostx64/x64/cl.exe"), "@" + str(response)], cwd=output).returncode


if __name__ == "__main__":
    raise SystemExit(main())
