param(
    [Parameter(Mandatory=$true)][string]$EngineRoot,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build\UE5.4')
)
$ErrorActionPreference = 'Stop'
$repository = $PSScriptRoot
$enginePath = (Resolve-Path -LiteralPath $EngineRoot).Path
$uat = Join-Path $enginePath 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path -LiteralPath $uat -PathType Leaf)) {
    throw "RunUAT.bat was not found under $enginePath"
}
$packagePath = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $packagePath) {
    throw "Build output already exists at $packagePath. Move that previous build before rerunning."
}
# Stage only plugin files. BuildPlugin copies its source directory recursively,
# so the package output must never be inside the source directory it copies.
$stagePath = Join-Path $repository ('build\staging\' + [guid]::NewGuid().ToString('N') + '\UAssetAgent')
New-Item -ItemType Directory -Path $stagePath -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repository 'UAssetAgent.uplugin') -Destination $stagePath
Copy-Item -LiteralPath (Join-Path $repository 'Source') -Destination $stagePath -Recurse
Copy-Item -LiteralPath (Join-Path $repository 'Config'),(Join-Path $repository 'LICENSE') -Destination $stagePath -Recurse
& $uat BuildPlugin "-Plugin=$stagePath\UAssetAgent.uplugin" "-Package=$packagePath" -TargetPlatforms=Win64
if ($LASTEXITCODE -ne 0) { throw "Unreal BuildPlugin failed with exit code $LASTEXITCODE" }
Write-Output "Built plugin: $packagePath"
