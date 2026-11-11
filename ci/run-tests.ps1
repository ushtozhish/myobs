# Runs the MyOBS automatic tests from the assembled runtime folder.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'runtime/bin/64bit'
Set-Location $bin
& (Join-Path $bin 'myobs-tests.exe')
$code = $LASTEXITCODE
Write-Host "myobs-tests exit code: $code"
exit $code
