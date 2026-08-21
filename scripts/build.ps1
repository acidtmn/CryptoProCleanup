param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('Win32', 'x64')]
    [string]$Platform = 'Win32'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'check-theme-resources.ps1')
& (Join-Path $PSScriptRoot 'check-layout-resources.ps1')
# Visual Studio can leave optional ATL/MFC directories in LIB even when that
# component is not installed. Remove only non-existent entries for this build
# process so WinUI's build tasks do not emit a misleading compiler warning.
if ($env:LIB) {
    $env:LIB = (($env:LIB -split ';') | Where-Object { $_ -and (Test-Path -LiteralPath $_) }) -join ';'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild with the C++ x86 toolchain was not found.' }

$msbuildArguments = @(
    (Join-Path $projectRoot 'CryptoProCleanup.sln'),
    '/m',
    '/t:Build',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    '/v:minimal'
)
if ($Platform -eq 'x64') {
    & $msbuild (Join-Path $projectRoot 'CryptoProCleanupModern.vcxproj') /t:Restore /p:RestorePackagesConfig=true "/p:RestoreRepositoryPath=$(Join-Path $projectRoot 'packages')" /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Modern dependency restore failed with exit code $LASTEXITCODE." }
}
& $msbuild @msbuildArguments
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE." }

$tests = Join-Path $projectRoot "build\$Platform\$Configuration\CryptoProCleanupTests.exe"
& $tests
if ($LASTEXITCODE -ne 0) { throw "Unit tests failed with exit code $LASTEXITCODE." }
