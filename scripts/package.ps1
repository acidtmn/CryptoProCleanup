param([string]$Version = '0.4.0')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release

$distRoot = Join-Path $projectRoot 'dist'
$packageRoot = Join-Path $distRoot "CryptoProCleanup-$Version"
$resolvedProject = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\')
$resolvedPackage = [IO.Path]::GetFullPath($packageRoot)
if (-not $resolvedPackage.StartsWith($resolvedProject + '\dist\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe package output path.' }
if (Test-Path -LiteralPath $packageRoot) { Remove-Item -LiteralPath $packageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

Copy-Item -LiteralPath (Join-Path $projectRoot 'build\Release\CryptoProCleanup.exe') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.en.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'CHANGELOG.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'SECURITY.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination $packageRoot -Recurse

$sourceStage = Join-Path $packageRoot 'source'
New-Item -ItemType Directory -Path $sourceStage -Force | Out-Null
foreach ($directory in @('.github', 'assets', 'src', 'tests', 'scripts', 'docs')) { Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $sourceStage -Recurse }
foreach ($file in @('.gitattributes', '.gitignore', 'CryptoProCleanup.sln', 'CryptoProCleanup.vcxproj', 'CryptoProCleanupTests.vcxproj', 'README.md', 'README.en.md', 'LICENSE', 'CHANGELOG.md', 'CONTRIBUTING.md', 'SECURITY.md')) { Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $sourceStage }

$sourceZip = Join-Path $packageRoot "CryptoProCleanup-$Version-source.zip"
Compress-Archive -Path (Join-Path $sourceStage '*') -DestinationPath $sourceZip -CompressionLevel Optimal
Remove-Item -LiteralPath $sourceStage -Recurse -Force

$hashLines = Get-ChildItem -LiteralPath $packageRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $relative = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
    "$hash  $relative"
}
[IO.File]::WriteAllLines((Join-Path $packageRoot 'SHA256SUMS.txt'), $hashLines, [Text.UTF8Encoding]::new($false))

$releaseZip = Join-Path $distRoot "CryptoProCleanup-$Version-win32.zip"
if (Test-Path -LiteralPath $releaseZip) { Remove-Item -LiteralPath $releaseZip -Force }
Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $releaseZip -CompressionLevel Optimal
Write-Host "Created $releaseZip"
