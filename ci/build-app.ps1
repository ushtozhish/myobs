# Builds MyOBS (app, own plugins, tests) against the installed engine and assembles a
# runnable folder: runtime/ (same layout as OBS: bin/64bit, data, obs-plugins/64bit).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $root 'engine-build/install'
$deps = Get-ChildItem (Join-Path $root 'engine-build/deps') -Directory
$prefix = @($engine) + ($deps | ForEach-Object { $_.FullName })
$prefixArg = ($prefix -join ';').Replace('\', '/')

# NDI SDK headers (NDI License) for the NDI input: taken at build time, pinned, not stored in the repository.
$ndiCommit = 'fa41b38f18f07acc3c041e382e091a196fc12db0'
$ndi = Join-Path $root 'engine-build/ndi-sdk'
if (-not (Test-Path (Join-Path $ndi 'lib/ndi/Processing.NDI.Lib.h'))) {
    if (Test-Path $ndi) { Remove-Item $ndi -Recurse -Force }
    git init -q $ndi
    git -C $ndi remote add origin https://github.com/DistroAV/DistroAV.git
    git -C $ndi sparse-checkout set lib/ndi
    git -C $ndi fetch -q --depth 1 --filter=blob:none origin $ndiCommit
    git -C $ndi checkout -q FETCH_HEAD
}
$ndiInclude = (Join-Path $ndi 'lib/ndi').Replace('\', '/')

cmake -S $root -B (Join-Path $root 'build') -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=$prefixArg" "-DMYOBS_NDI_SDK_DIR=$ndiInclude"
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
# runtime DLLs of the engine dependencies (FFmpeg, zlib, x264...) - OBS copies them in its own build, we do it here
$bin = Join-Path $runtime 'bin/64bit'
foreach ($d in $deps) {
    if ($d.Name -like 'obs-deps-qt6-*') { continue }
    $db = Join-Path $d.FullName 'bin'
    if (Test-Path $db) { Get-ChildItem $db -Filter *.dll | ForEach-Object { if (-not (Test-Path (Join-Path $bin $_.Name))) { Copy-Item $_.FullName $bin } } }
}
Get-ChildItem $bin -Filter *.dll | ForEach-Object { $_.Name } | Sort-Object | Write-Host
Write-Host 'MyOBS assembled in runtime/'
