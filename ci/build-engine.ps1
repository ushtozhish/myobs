# Builds the OBS engine (libobs + plugins, without the OBS user interface) at the pinned
# version, with the MyOBS patches applied, and installs it to engine-build/install.
# The prebuilt dependencies OBS downloads (FFmpeg, Qt, ...) are kept in engine-build/deps.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root 'engine-build'
$version = (Get-Content (Join-Path $root 'engine/OBS_VERSION')).Trim()
New-Item -ItemType Directory -Force -Path $work | Out-Null
Set-Location $work

if (-not (Test-Path 'obs-studio')) {
    git -c core.autocrlf=false clone --depth 1 --branch $version --recurse-submodules --shallow-submodules https://github.com/obsproject/obs-studio obs-studio
    if ($LASTEXITCODE) { throw "clone of OBS $version failed" }
}
Set-Location obs-studio
foreach ($patch in Get-ChildItem (Join-Path $root 'engine/patches/*.patch') | Sort-Object Name) {
    Write-Host "patch: $($patch.Name)"
    git apply --ignore-whitespace --whitespace=nowarn $patch.FullName
    if ($LASTEXITCODE) { throw "patch $($patch.Name) does not apply" }
}

cmake --preset windows-x64 `
    -DENABLE_FRONTEND=OFF -DENABLE_BROWSER=OFF -DENABLE_WEBSOCKET=OFF -DENABLE_SCRIPTING=OFF `
    -DENABLE_AJA=OFF -DENABLE_VLC=OFF
if ($LASTEXITCODE) { throw 'engine configure failed' }
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
if ($LASTEXITCODE) { throw 'engine build failed' }

$install = Join-Path $work 'install'
cmake --install build_x64 --prefix $install --config RelWithDebInfo
if ($LASTEXITCODE) { throw 'engine install failed' }
cmake --install build_x64 --prefix $install --config RelWithDebInfo --component Development
if ($LASTEXITCODE) { throw 'engine development install failed' }

$deps = Join-Path $work 'deps'
New-Item -ItemType Directory -Force -Path $deps | Out-Null
Get-ChildItem .deps -Directory | Where-Object { $_.Name -like 'obs-deps-*' } | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $deps $_.Name) -Recurse -Force
}
Write-Host 'engine ready'
