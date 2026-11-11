# Builds MyOBS (app, own plugins, tests) against the installed engine and assembles a
# runnable folder: runtime/ (same layout as OBS: bin/64bit, data, obs-plugins/64bit).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $root 'engine-build/install'
$deps = Get-ChildItem (Join-Path $root 'engine-build/deps') -Directory
$prefix = @($engine) + ($deps | ForEach-Object { $_.FullName })
$prefixArg = ($prefix -join ';').Replace('\', '/')

cmake -S $root -B (Join-Path $root 'build') -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=$prefixArg"
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build (Join-Path $root 'build') --config RelWithDebInfo --parallel
if ($LASTEXITCODE) { throw 'build failed' }

$runtime = Join-Path $root 'runtime'
if (Test-Path $runtime) { Remove-Item $runtime -Recurse -Force }
Copy-Item $engine $runtime -Recurse
# Engine development files are not needed at run time.
foreach ($d in 'include', 'lib', 'cmake') { $p = Join-Path $runtime $d; if (Test-Path $p) { Remove-Item $p -Recurse -Force } }
cmake --install (Join-Path $root 'build') --prefix $runtime --config RelWithDebInfo
if ($LASTEXITCODE) { throw 'install failed' }

# Qt for the app.
$qt = $deps | Where-Object { $_.Name -like 'obs-deps-qt6-*' } | Select-Object -First 1
$windeployqt = Join-Path $qt.FullName 'bin/windeployqt.exe'
if (Test-Path $windeployqt) {
    & $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $runtime 'bin/64bit/myobs.exe')
    if ($LASTEXITCODE) { throw 'windeployqt failed' }
} else {
    Write-Warning 'windeployqt not found; copying Qt DLLs directly'
    Copy-Item (Join-Path $qt.FullName 'bin/Qt6Core.dll'), (Join-Path $qt.FullName 'bin/Qt6Gui.dll'), (Join-Path $qt.FullName 'bin/Qt6Widgets.dll') (Join-Path $runtime 'bin/64bit')
    New-Item -ItemType Directory -Force -Path (Join-Path $runtime 'bin/64bit/platforms') | Out-Null
    Copy-Item (Join-Path $qt.FullName 'plugins/platforms/qwindows.dll') (Join-Path $runtime 'bin/64bit/platforms')
}
Write-Host 'MyOBS assembled in runtime/'
