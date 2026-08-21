param(
    [string]$Executable,
    [string]$ArtifactsDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $projectRoot 'build\Win32\Release\CryptoProCleanupLegacy.exe' }
if (-not $ArtifactsDirectory) { $ArtifactsDirectory = Join-Path $projectRoot 'artifacts\legacy-ui-smoke\0.5.3-rc1' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Legacy executable was not found: $Executable" }

$settingsPath = 'HKCU:\Software\CodeAlexandrov\CryptoProCleanup\Legacy'
$settingsExisted = Test-Path -LiteralPath $settingsPath
$savedSettings = @{}
if ($settingsExisted) {
    $settingsKey = Get-Item -LiteralPath $settingsPath
    foreach ($name in $settingsKey.GetValueNames()) {
        $savedSettings[$name] = @($settingsKey.GetValue($name, $null, 'DoNotExpandEnvironmentNames'),
                                  $settingsKey.GetValueKind($name).ToString())
    }
}

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CpcLegacySmokeNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rectangle);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@

function Wait-Until([scriptblock]$Condition, [int]$Seconds = 45) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Select-Page([IntPtr]$Window, [int]$ControlId) {
    $control = [CpcLegacySmokeNative]::GetDlgItem($Window, $ControlId)
    [void][CpcLegacySmokeNative]::SendMessage($Window, 0x0111, [IntPtr]$ControlId, $control)
    Start-Sleep -Milliseconds 350
}

function Assert-ActionsInsideWindow([IntPtr]$Window, [int[]]$ControlIds) {
    $windowRectangle = [CpcLegacySmokeNative+RECT]::new()
    if (-not [CpcLegacySmokeNative]::GetWindowRect($Window, [ref]$windowRectangle)) { throw 'GetWindowRect failed.' }
    foreach ($id in $ControlIds) {
        $control = [CpcLegacySmokeNative]::GetDlgItem($Window, $id)
        $rectangle = [CpcLegacySmokeNative+RECT]::new()
        if ($control -eq [IntPtr]::Zero -or
            -not [CpcLegacySmokeNative]::IsWindowVisible($control) -or
            -not [CpcLegacySmokeNative]::GetWindowRect($control, [ref]$rectangle) -or
            $rectangle.Left -lt $windowRectangle.Left -or $rectangle.Top -lt $windowRectangle.Top -or
            $rectangle.Right -gt $windowRectangle.Right -or $rectangle.Bottom -gt $windowRectangle.Bottom -or
            ($rectangle.Right - $rectangle.Left) -lt 80 -or ($rectangle.Bottom - $rectangle.Top) -lt 20) {
            throw ("Legacy action is clipped at the current size: {0}; window={1},{2},{3},{4}; control={5},{6},{7},{8}" -f
                $id, $windowRectangle.Left, $windowRectangle.Top, $windowRectangle.Right, $windowRectangle.Bottom,
                $rectangle.Left, $rectangle.Top, $rectangle.Right, $rectangle.Bottom)
        }
    }
}

function Assert-WindowSize([IntPtr]$Window, [int]$Width, [int]$Height) {
    $rectangle = [CpcLegacySmokeNative+RECT]::new()
    if (-not [CpcLegacySmokeNative]::GetWindowRect($Window, [ref]$rectangle)) { throw 'GetWindowRect failed.' }
    if (($rectangle.Right - $rectangle.Left) -ne $Width -or ($rectangle.Bottom - $rectangle.Top) -ne $Height) {
        throw "Legacy window did not reach the requested ${Width}x${Height} test size."
    }
}

function Assert-ActionsDoNotOverlap([IntPtr]$Window, [int[]]$ControlIds) {
    $rectangles = @()
    foreach ($id in $ControlIds) {
        $rectangle = [CpcLegacySmokeNative+RECT]::new()
        if (-not [CpcLegacySmokeNative]::GetWindowRect(
                [CpcLegacySmokeNative]::GetDlgItem($Window, $id), [ref]$rectangle)) {
            throw "GetWindowRect failed for Legacy action: $id"
        }
        $rectangles += [pscustomobject]@{ Id = $id; Rectangle = $rectangle }
    }
    for ($leftIndex = 0; $leftIndex -lt $rectangles.Count; $leftIndex++) {
        for ($rightIndex = $leftIndex + 1; $rightIndex -lt $rectangles.Count; $rightIndex++) {
            $leftRectangle = $rectangles[$leftIndex].Rectangle
            $rightRectangle = $rectangles[$rightIndex].Rectangle
            if ($leftRectangle.Left -lt $rightRectangle.Right -and
                $leftRectangle.Right -gt $rightRectangle.Left -and
                $leftRectangle.Top -lt $rightRectangle.Bottom -and
                $leftRectangle.Bottom -gt $rightRectangle.Top) {
                throw "Legacy actions overlap: $($rectangles[$leftIndex].Id) and $($rectangles[$rightIndex].Id)"
            }
        }
    }
}

function Save-WindowScreenshot([IntPtr]$Window, [string]$Name) {
    New-Item -ItemType Directory -Path $ArtifactsDirectory -Force | Out-Null
    [void][CpcLegacySmokeNative]::ShowWindow($Window, 5)
    [void][CpcLegacySmokeNative]::SetForegroundWindow($Window)
    $rectangle = [CpcLegacySmokeNative+RECT]::new()
    if (-not [CpcLegacySmokeNative]::GetWindowRect($Window, [ref]$rectangle)) { throw 'GetWindowRect failed.' }
    [void][CpcLegacySmokeNative]::SetWindowPos(
        $Window, [IntPtr](-1), $rectangle.Left, $rectangle.Top,
        $rectangle.Right - $rectangle.Left, $rectangle.Bottom - $rectangle.Top, 0)
    Start-Sleep -Milliseconds 200
    $bitmap = [System.Drawing.Bitmap]::new($rectangle.Right - $rectangle.Left, $rectangle.Bottom - $rectangle.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rectangle.Left, $rectangle.Top, 0, 0, $bitmap.Size)
        $bitmap.Save((Join-Path $ArtifactsDirectory "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    [void][CpcLegacySmokeNative]::SetWindowPos(
        $Window, [IntPtr](-2), $rectangle.Left, $rectangle.Top,
        $rectangle.Right - $rectangle.Left, $rectangle.Bottom - $rectangle.Top, 0)
}

$process = Start-Process -FilePath $Executable -ArgumentList '--lang','ru' -PassThru
try {
    if (-not (Wait-Until { $process.Refresh(); $process.MainWindowHandle -ne 0 -and $process.Responding })) {
        throw 'Legacy did not expose a responsive main window.'
    }
    $window = $process.MainWindowHandle
    [void][CpcLegacySmokeNative]::SetWindowPos($window, [IntPtr]::Zero, 20, 20, 800, 600, 0x0004)
    Assert-WindowSize $window 800 600
    if (-not (Wait-Until { [CpcLegacySmokeNative]::IsWindowEnabled(
            [CpcLegacySmokeNative]::GetDlgItem($window, 1103)) })) {
        throw 'Legacy navigation did not become available after the initial safe scan.'
    }

    Assert-ActionsInsideWindow $window @(1019, 1012, 1013)
    Save-WindowScreenshot $window 'overview-800x600-ru'
    Select-Page $window 1103
    Assert-ActionsInsideWindow $window @(1144, 1028)
    Save-WindowScreenshot $window 'certificates-800x600-ru'
    Select-Page $window 1104
    Assert-ActionsInsideWindow $window @(1040, 1046, 1041, 1042)
    Save-WindowScreenshot $window 'offline-800x600-ru'

    [void][CpcLegacySmokeNative]::SetWindowPos($window, [IntPtr]::Zero, 20, 20, 1024, 768, 0x0004)
    Select-Page $window 1102
    Assert-ActionsInsideWindow $window @(1019, 1012, 1013)
    Save-WindowScreenshot $window 'overview-1024x768-ru'

    [void][CpcLegacySmokeNative]::SetWindowPos($window, [IntPtr]::Zero, 20, 20, 768, 528, 0x0004)
    Assert-WindowSize $window 768 528
    Assert-ActionsInsideWindow $window @(1019, 1012, 1013)
    Save-WindowScreenshot $window 'overview-800x600-workarea-ru'
    Select-Page $window 1103
    Assert-ActionsInsideWindow $window @(1144, 1028)
    Save-WindowScreenshot $window 'certificates-800x600-workarea-ru'
    Select-Page $window 1104
    Assert-ActionsInsideWindow $window @(1040, 1046, 1041, 1042)
    Save-WindowScreenshot $window 'offline-800x600-workarea-ru'
    Select-Page $window 1105
    Assert-ActionsInsideWindow $window @(1161, 1162, 1163)
    Assert-ActionsDoNotOverlap $window @(1161, 1162, 1163)
    Save-WindowScreenshot $window 'reports-800x600-workarea-ru'
    Select-Page $window 1106
    Assert-ActionsInsideWindow $window @(1176)
    Save-WindowScreenshot $window 'settings-800x600-workarea-ru'
    Select-Page $window 1107
    Assert-ActionsInsideWindow $window @(1182)
    Save-WindowScreenshot $window 'about-800x600-workarea-ru'
    Write-Host "Legacy UI smoke passed at 800x600 and 1024x768: $ArtifactsDirectory"
} finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(10000)) { Stop-Process -Id $process.Id -Force }
    }
    if (Test-Path -LiteralPath $settingsPath) { Remove-Item -LiteralPath $settingsPath -Recurse -Force }
    if ($settingsExisted) {
        New-Item -Path $settingsPath -Force | Out-Null
        foreach ($name in $savedSettings.Keys) {
            $value, $kind = $savedSettings[$name]
            $propertyType = if ($kind -eq 'DWord') { 'DWord' } elseif ($kind -eq 'QWord') { 'QWord' }
                            elseif ($kind -eq 'ExpandString') { 'ExpandString' } else { 'String' }
            New-ItemProperty -Path $settingsPath -Name $name -Value $value -PropertyType $propertyType -Force | Out-Null
        }
    }
}
