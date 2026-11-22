# Publish the Win64 buyer ZIP

A push to `main` runs the Python tests on GitHub-hosted Windows. To release that commit, run the publisher on your Windows build PC. Use the same PC and Windows account for all publications so the local release lock serializes updates to Latest.

## One-time setup

1. Install **Unreal Engine 5.4.4** through the Epic Games Launcher Library. Launch the editor once to finish prerequisite installation. The publisher checks the registered Launcher installation and the editor module BuildId. [Epic installation guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/install-unreal-engine?application_version=5.4)
2. Install Git, GitHub CLI and Visual Studio 2022 with **Game development with C++**, a UE 5.4-compatible MSVC v143 toolset and the Windows SDK. The build uses Unreal's bundled Python. [Epic toolchain guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-visual-studio-for-unreal-engine?application_version=5.4)
3. From PowerShell, sign in with an account that can publish releases and clone a release checkout into a short writable path. [GitHub CLI login](https://cli.github.com/manual/gh_auth_login)

   ```powershell
   gh auth login --hostname github.com --web --git-protocol https
   git clone --branch main https://github.com/TheEugen/UAssetEditorMCP.git C:\src\UAssetRelease
   ```

## After pushing a commit to main

In PowerShell, use your release checkout. At the prompt, enter the registered Launcher installation directory that contains `Engine`.

```powershell
Set-Location C:\src\UAssetRelease
git pull --ff-only origin main
if ($LASTEXITCODE) { throw 'Could not update the release checkout' }
$engine = Read-Host 'Epic Launcher UE 5.4.4 installation directory'
& (Join-Path $engine 'Engine\Binaries\ThirdParty\Python3\Win64\python.exe') -B -X utf8 tools/publish_release.py --engine $engine
```

The command verifies that HEAD matches GitHub's current `main` and builds a fresh copy of that exact commit. It runs the unit tests and `build-plugin.ps1`, extracts the ZIP, checks the DLL and module metadata, installs the plugin into a disposable project and calls the packaged server to read `blueprint_protocol`, create a Blueprint and inspect it with Unreal.

The validated ZIP is written to `dist/<full SHA>/UAssetEditorMCP-UE5.4.4-Win64.zip`. The publisher uploads it to a draft, verifies GitHub's size and SHA-256, then publishes the commit-tagged release. An older commit on the same history cannot take Latest from a newer release. If the command fails, fix the reported error and run it again. A retry can repair a draft and preserves the original asset for an already published commit.

Check the Latest tag and asset in PowerShell.

```powershell
gh release view --repo TheEugen/UAssetEditorMCP --json tagName,assets --jq '{tag: .tagName, files: [.assets[].name]}'
```

The tag ends in the released commit's full SHA. Use the stable download link in the Agensi listing after the first successful publication. [Download the latest Win64 ZIP](https://github.com/TheEugen/UAssetEditorMCP/releases/latest/download/UAssetEditorMCP-UE5.4.4-Win64.zip)
