# Runs the MyOBS automatic tests from the assembled runtime folder.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'runtime/bin/64bit'
Set-Location $bin
& (Join-Path $bin 'myobs-drift-test.exe')
$drift = $LASTEXITCODE
Write-Host "myobs-drift-test exit code: $drift"
& (Join-Path $bin 'myobs-tests.exe')
$code = $LASTEXITCODE
Write-Host "myobs-tests exit code: $code"
if ($drift) { exit $drift }
exit $code
