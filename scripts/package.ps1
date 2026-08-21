param([string]$Version)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $versionHeader = Get-Content -LiteralPath (Join-Path $projectRoot 'src\version.hpp') -Raw
    $match = [regex]::Match($versionHeader, '#define\s+CPC_VERSION_TEXT\s+"([^"]+)"')
    if (-not $match.Success) { throw 'The centralized version could not be read from src\version.hpp.' }
    $Version = $match.Groups[1].Value
}
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -Platform Win32
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -Platform x64

$distRoot = Join-Path $projectRoot 'dist'
$packageRoot = Join-Path $distRoot "CryptoProCleanup-$Version"
$resolvedProject = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\')
$resolvedPackage = [IO.Path]::GetFullPath($packageRoot)
if (-not $resolvedPackage.StartsWith($resolvedProject + '\dist\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe package output path.' }
if (Test-Path -LiteralPath $packageRoot) { Remove-Item -LiteralPath $packageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

$modernBuild = Join-Path $projectRoot 'build\x64\Release\Modern'
$modernStage = Join-Path $packageRoot 'Modern-Windows10-11-x64'
$legacyStage = Join-Path $packageRoot 'Legacy-Windows7-11-x86'
New-Item -ItemType Directory -Path $modernStage -Force | Out-Null
New-Item -ItemType Directory -Path $legacyStage -Force | Out-Null
foreach ($item in Get-ChildItem -LiteralPath $modernBuild -Force) {
    if ($item.Name -in @('CryptoProCleanupModern.exe', 'CryptoProCleanupModern.lib', 'CryptoProCleanupModern.exp', 'CryptoProCleanupModern.winmd')) { continue }
    if (-not $item.PSIsContainer -and $item.Extension -in @('.pdb', '.lib', '.exp')) { continue }
    if ($item.Name -eq 'src') { continue }
    if ($item.Name -like 'Microsoft.Web.WebView2*' -or $item.Name -eq 'WebView2Loader.dll') { continue }
    Copy-Item -LiteralPath $item.FullName -Destination $modernStage -Recurse
}
if (-not (Test-Path -LiteralPath (Join-Path $modernStage 'CryptoProCleanup.exe'))) { throw 'Modern executable is missing from the package.' }
if (-not (Test-Path -LiteralPath (Join-Path $modernStage 'CryptoProCleanupResume.exe'))) { throw 'Native resume helper is missing from the package.' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'build\Win32\Release\CryptoProCleanupLegacy.exe') -Destination $legacyStage
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.en.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'CHANGELOG.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'SECURITY.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination $packageRoot -Recurse

$sourceStage = Join-Path $packageRoot 'source'
New-Item -ItemType Directory -Path $sourceStage -Force | Out-Null
foreach ($directory in @('.github', 'assets', 'src', 'tests', 'scripts', 'docs')) { Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $sourceStage -Recurse }
foreach ($file in @('.gitattributes', '.gitignore', 'App.xaml', 'App.xaml.cpp', 'App.xaml.h', 'MainWindow.xaml', 'MainWindow.xaml.cpp', 'MainWindow.xaml.h', 'packages.config', 'CryptoProCleanup.sln', 'CryptoProCleanup.vcxproj', 'CryptoProCleanupModern.vcxproj', 'CryptoProCleanupResume.vcxproj', 'CryptoProCleanupTests.vcxproj', 'README.md', 'README.en.md', 'LICENSE', 'CHANGELOG.md', 'CONTRIBUTING.md', 'SECURITY.md')) { Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $sourceStage }

$sourceZip = Join-Path $packageRoot "CryptoProCleanup-$Version-source.zip"
Compress-Archive -Path (Join-Path $sourceStage '*') -DestinationPath $sourceZip -CompressionLevel Optimal
Remove-Item -LiteralPath $sourceStage -Recurse -Force

$hashLines = Get-ChildItem -LiteralPath $packageRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $relative = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
    "$hash  $relative"
}
[IO.File]::WriteAllLines((Join-Path $packageRoot 'SHA256SUMS.txt'), $hashLines, [Text.UTF8Encoding]::new($false))

$releaseZip = Join-Path $distRoot "CryptoProCleanup-$Version-windows.zip"
if (Test-Path -LiteralPath $releaseZip) { Remove-Item -LiteralPath $releaseZip -Force }
Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $releaseZip -CompressionLevel Optimal

$modernZip = Join-Path $distRoot "CryptoProCleanup-Modern-x64-$Version.zip"
if (Test-Path -LiteralPath $modernZip) { Remove-Item -LiteralPath $modernZip -Force }
Compress-Archive -Path (Join-Path $modernStage '*') -DestinationPath $modernZip -CompressionLevel Optimal

$legacyExe = Join-Path $distRoot "CryptoProCleanup-Legacy-x86-$Version.exe"
if (Test-Path -LiteralPath $legacyExe) { Remove-Item -LiteralPath $legacyExe -Force }
Copy-Item -LiteralPath (Join-Path $legacyStage 'CryptoProCleanupLegacy.exe') -Destination $legacyExe

$publicSourceZip = Join-Path $distRoot "CryptoProCleanup-$Version-source.zip"
if (Test-Path -LiteralPath $publicSourceZip) { Remove-Item -LiteralPath $publicSourceZip -Force }
Copy-Item -LiteralPath $sourceZip -Destination $publicSourceZip

$publicSums = Join-Path $distRoot "SHA256SUMS-$Version.txt"
$releaseAssets = @($releaseZip, $modernZip, $legacyExe, $publicSourceZip)
$releaseHashLines = $releaseAssets | Sort-Object | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($_))"
}
[IO.File]::WriteAllLines($publicSums, $releaseHashLines, [Text.UTF8Encoding]::new($false))

Write-Host 'Created release assets:'
$releaseAssets + $publicSums | ForEach-Object { Write-Host "  $_" }
