"""Optional Windows API/syntax check using installed UE headers (not a UBT build).

UHT and a runnable plugin still require the normal Unreal build. --link also
checks imports against UE libraries. Outputs stay inside this repository. The
check substitutes only our reflected class declaration.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--link", action="store_true", help="Also verify imports against UE libraries (still not a UHT/UBT build)")
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
    imports = "#undef BLUEPRINTGRAPH_API\n#define BLUEPRINTGRAPH_API DLLIMPORT\n" + "\n".join(f"#ifndef {module}_API\n#define {module}_API DLLIMPORT\n#endif" for module in
                        ("ANIMGRAPH", "ANIMGRAPHRUNTIME", "UMG", "UMGEDITOR", "SLATECORE", "MOVIESCENE", "ASSETTOOLS", "MOVIESCENETRACKS", "SEQUENCER"))
    translation.write_text(imports + "\n" + source.read_text(encoding="utf-8").replace('#include "UAssetAgentCommandlet.h"', declaration), encoding="utf-8")
    includes = [source.parent, args.msvc / "include", args.sdk / "ucrt", args.sdk / "shared", args.sdk / "um", args.sdk / "winrt",
                engine / "Source", engine / "Source/Runtime", engine / "Source/Runtime/Core/Public/Windows",
                engine / "Shaders/Shared"]
    for folder in (engine / "Source/Runtime", engine / "Source/Editor", engine / "Source/Developer"):
        for name in ("Public", "Classes"):
            includes.extend(folder.rglob(name))
    includes.extend((engine / "Intermediate/Build/Win64/UnrealEditor/Inc").glob("*/UHT"))
    options = ["/nologo", "/c" if args.link else "/Zs", "/MD", "/std:c++20", "/EHsc", "/bigobj", "/utf-8", "/permissive-", "/Zc:__cplusplus",
               '/FI"' + str(definitions) + '"']
    options.extend('/I"' + str(path) + '"' for path in includes)
    options.append('"' + str(translation) + '"')
    response = output / "compile.rsp"
    response.write_text("\n".join(options), encoding="utf-8")
    status = subprocess.run([str(args.msvc / "bin/Hostx64/x64/cl.exe"), "@" + str(response)], cwd=output).returncode
    if status or not args.link:
        return status
    modules = ("Core", "CoreUObject", "Engine", "UnrealEd", "BlueprintGraph", "Kismet", "KismetCompiler", "Json",
               "AssetRegistry", "AnimGraph", "AnimGraphRuntime", "UMG", "UMGEditor", "SlateCore", "MovieScene", "AssetTools")
    libraries = [engine / f"Intermediate/Build/Win64/x64/UnrealEditor/Development/{module}/UnrealEditor-{module}.lib" for module in modules]
    link_options = ["/NOLOGO", "/DLL", "/OUT:ApiCheck.dll", "Check.obj", "ucrt.lib", "vcruntime.lib", "msvcrt.lib", "kernel32.lib",
                    f'/LIBPATH:"{args.msvc / "lib/x64"}"',
                    f'/LIBPATH:"{args.sdk.parent.parent / "Lib" / args.sdk.name / "um/x64"}"',
                    f'/LIBPATH:"{args.sdk.parent.parent / "Lib" / args.sdk.name / "ucrt/x64"}"']
    link_options.extend(f'"{path}"' for path in libraries)
    link_response = output / "link.rsp"
    link_response.write_text("\n".join(link_options), encoding="utf-8")
    return subprocess.run([str(args.msvc / "bin/Hostx64/x64/link.exe"), "@" + str(link_response)], cwd=output).returncode


if __name__ == "__main__":
    raise SystemExit(main())
