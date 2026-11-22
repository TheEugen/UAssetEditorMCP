# Win64 release runner

GitHub runs the build and verification on a dedicated Windows x64 PC or VM, then publishes the validated artifact from a GitHub-hosted Ubuntu runner. The machine with Unreal only needs read access to the repository.

## One-time setup

1. On the dedicated Windows machine, use a licensed Epic Games Launcher account to install Unreal Engine **5.4.4** from the Library. Launch the editor once to finish installing prerequisites. Retain the Launcher installation registration. The build checks the Launcher inventory, exact engine version, installed-build marker and editor module BuildId. [Epic installation guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/install-unreal-engine?application_version=5.4)
2. Install Git and Visual Studio 2022 with **Game development with C++**, the UE 5.4-compatible MSVC v143 toolset and Windows SDK. The workflow uses the Python interpreter included with Unreal. [Epic toolchain guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-visual-studio-for-unreal-engine?application_version=5.4)
3. Set the machine environment variable `UE_ENGINE_ROOT` to the Launcher directory that contains `Engine`. Give the build account access to that directory, its build caches and a short writable runner work path. Restart the runner service after changing its environment.
4. Open **Settings → Actions → Runners → New self-hosted runner** in this repository. Select **Windows / x64** and run GitHub's registration commands on the dedicated machine. Keep the default labels and add the custom label **`ue-5.4.4-launcher`**. Run it as a service and confirm its status is **Idle**. Reserve the machine for trusted builds of this repository. [GitHub runner registration](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners)
5. Enable GitHub Actions and permit the publishing job's `contents: write` permission. The workflow runs on pushes to `main`. A queued build can start when the runner connects. An expired run can be rerun from Actions.

## Build and publication

The workflow checks out the push SHA and calls `tools/package_release.py`, which runs `build-plugin.ps1`. It extracts the finished ZIP, installs it in a disposable project and talks to the extracted MCP server over stdio. Verification reads the complete protocol, creates a fixture Blueprint and inspects it in another editor process. The SHA-256 of that exact ZIP travels with the Actions run.

The publish job queues with other publishers. It uploads to a draft tagged `ue5.4.4-win64-<full SHA>`, checks GitHub's asset size and digest, then publishes. The release notes identify the commit and ZIP checksum. The commit graph orders releases on the same line of history; for divergent histories the push workflow run number breaks the tie. Reruns keep the same tag and reuse a complete published asset. Incomplete draft uploads can be retried. [GitHub release API](https://docs.github.com/en/rest/releases/releases) · [Workflow concurrency](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#concurrency)

Use the [stable buyer download](https://github.com/TheEugen/UAssetEditorMCP/releases/latest/download/UAssetEditorMCP-UE5.4.4-Win64.zip) for the Agensi listing. It becomes available with the first successful publication. Reserve Latest for these Win64 releases so the link continues to resolve.
