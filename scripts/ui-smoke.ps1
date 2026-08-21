param(
    [string]$Executable,
    [string]$ArtifactsDirectory,
    [switch]$NoScreenshots
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $projectRoot 'build\x64\Release\Modern\CryptoProCleanup.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Modern executable was not found: $Executable" }
$versionHeader = Get-Content -LiteralPath (Join-Path $projectRoot 'src\version.hpp') -Raw
$versionMatch = [regex]::Match($versionHeader, '#define\s+CPC_VERSION_TEXT\s+"([^"]+)"')
if (-not $versionMatch.Success) { throw 'The centralized version could not be read.' }
$expectedVersion = $versionMatch.Groups[1].Value
if (-not $ArtifactsDirectory) { $ArtifactsDirectory = Join-Path $projectRoot "artifacts\ui-smoke\$expectedVersion" }

$settingsPath = 'HKCU:\Software\CodeAlexandrov\CryptoProCleanup\ModernWinUI'
$settingsExisted = Test-Path -LiteralPath $settingsPath
$savedSettings = @{}
if ($settingsExisted) {
    $settingsKey = Get-Item -LiteralPath $settingsPath
    foreach ($name in $settingsKey.GetValueNames()) {
        $savedSettings[$name] = @($settingsKey.GetValue($name, $null, 'DoNotExpandEnvironmentNames'), $settingsKey.GetValueKind($name).ToString())
    }
}

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CpcSmokeNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
'@

function Wait-Until([scriptblock]$Condition, [int]$Seconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Select-UiItem([System.Windows.Automation.AutomationElement]$Root, [string]$Name) {
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::IsEnabledProperty, $true))
    $item = $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if (-not $item) { return $false }
    foreach ($pattern in @([System.Windows.Automation.SelectionItemPattern]::Pattern,
                           [System.Windows.Automation.InvokePattern]::Pattern)) {
        try {
            $implementation = $item.GetCurrentPattern($pattern)
            if ($pattern -eq [System.Windows.Automation.SelectionItemPattern]::Pattern) { $implementation.Select() }
            else { $implementation.Invoke() }
            return $true
        } catch {}
    }
    return $false
}

function Activate-UiElement([System.Windows.Automation.AutomationElement]$Element) {
    if (-not $Element -or -not $Element.Current.IsEnabled) { return $false }
    foreach ($pattern in @([System.Windows.Automation.SelectionItemPattern]::Pattern,
                           [System.Windows.Automation.InvokePattern]::Pattern)) {
        try {
            $implementation = $Element.GetCurrentPattern($pattern)
            if ($pattern -eq [System.Windows.Automation.SelectionItemPattern]::Pattern) { $implementation.Select() }
            else { $implementation.Invoke() }
            return $true
        } catch {}
    }
    return $false
}

function Select-ComboValue([System.Windows.Automation.AutomationElement]$Combo, [string]$ItemName) {
    if ([string]::IsNullOrWhiteSpace($ItemName)) { throw 'Combo-box item name is empty.' }
    try { $Combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand() } catch {}
    $desktop = [System.Windows.Automation.AutomationElement]::RootElement
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $ItemName))
    $item = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $item = $desktop.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
        if ($item) { break }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $item) { throw "Combo-box item was not found: $ItemName" }
    $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}

function Find-UiElementById([System.Windows.Automation.AutomationElement]$Root, [string]$AutomationId) {
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $AutomationId))
}

function Select-UiItemById([System.Windows.Automation.AutomationElement]$Root, [string]$AutomationId) {
    $element = Find-UiElementById $Root $AutomationId
    if (-not $element -and $AutomationId.StartsWith('Nav')) {
        $element = Find-UiElementById $Root ('Compact' + $AutomationId.Substring(3))
    }
    return Activate-UiElement $element
}

function Assert-UiElementFullyVisible([System.Windows.Automation.AutomationElement]$Root, [string]$Name) {
    $element = $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $Name))
    if (-not $element) { throw "Expected visible action was not found: $Name" }
    if ($element.Current.IsOffscreen) {
        try { $element.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView() } catch {}
        Start-Sleep -Milliseconds 250
    }
    if ($element.Current.IsOffscreen) { throw "Action cannot be brought into view: $Name" }
    $windowRectangle = $Root.Current.BoundingRectangle
    $rectangle = $element.Current.BoundingRectangle
    if ($rectangle.Width -le 0 -or $rectangle.Height -le 0 -or
        $rectangle.Left -lt $windowRectangle.Left -or $rectangle.Top -lt $windowRectangle.Top -or
        $rectangle.Right -gt $windowRectangle.Right -or $rectangle.Bottom -gt $windowRectangle.Bottom) {
        throw "Action is clipped by the window: $Name"
    }
}

function Assert-CardInset([System.Windows.Automation.AutomationElement]$Root, [IntPtr]$Handle,
                          [string]$CardId, [string]$FirstId, [string]$LastId) {
    $card = Find-UiElementById $Root $CardId
    $first = Find-UiElementById $Root $FirstId
    $last = Find-UiElementById $Root $LastId
    if (-not $card -or -not $first -or -not $last) { throw "Card geometry element is missing: $CardId" }
    if ($card.Current.IsOffscreen -or $first.Current.IsOffscreen -or $last.Current.IsOffscreen) { return }
    $scale = [Math]::Max(1.0, [CpcSmokeNative]::GetDpiForWindow($Handle) / 96.0)
    $minimum = 12.0 * $scale - 2.0
    $cardRectangle = $card.Current.BoundingRectangle
    $firstRectangle = $first.Current.BoundingRectangle
    $lastRectangle = $last.Current.BoundingRectangle
    if (($firstRectangle.Left - $cardRectangle.Left) -lt $minimum -or
        ($firstRectangle.Top - $cardRectangle.Top) -lt $minimum -or
        ($cardRectangle.Right - $lastRectangle.Right) -lt $minimum -or
        ($cardRectangle.Bottom - $lastRectangle.Bottom) -lt $minimum) {
        throw "Card inset is below 12 DIP: $CardId"
    }
}

function Assert-UiElementMinimumWidth([System.Windows.Automation.AutomationElement]$Root, [IntPtr]$Handle,
                                      [string]$AutomationId, [double]$MinimumDip) {
    $element = Find-UiElementById $Root $AutomationId
    if (-not $element) { throw "Width-check element is missing: $AutomationId" }
    if ($element.Current.IsOffscreen) {
        try { $element.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView() } catch {}
        Start-Sleep -Milliseconds 200
    }
    $scale = [Math]::Max(1.0, [CpcSmokeNative]::GetDpiForWindow($Handle) / 96.0)
    $widthDip = $element.Current.BoundingRectangle.Width / $scale
    if ($widthDip -lt $MinimumDip) {
        throw "Action width can clip its full label: $AutomationId ($([Math]::Round($widthDip, 1)) DIP)"
    }
}

function Assert-UiElementMinimumHeight([System.Windows.Automation.AutomationElement]$Root, [IntPtr]$Handle,
                                       [string]$Name, [double]$MinimumDip) {
    $element = $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $Name))
    if (-not $element) { throw "Height-check element is missing: $Name" }
    if ($element.Current.IsOffscreen) {
        try { $element.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView() } catch {}
        Start-Sleep -Milliseconds 200
    }
    $scale = [Math]::Max(1.0, [CpcSmokeNative]::GetDpiForWindow($Handle) / 96.0)
    $heightDip = $element.Current.BoundingRectangle.Height / $scale
    if ($heightDip -lt $MinimumDip) {
        throw "Action height can clip its label: $Name ($([Math]::Round($heightDip, 1)) DIP)"
    }
}

function Assert-UiElementMinimumHeightById([System.Windows.Automation.AutomationElement]$Root, [IntPtr]$Handle,
                                           [string]$AutomationId, [double]$MinimumDip) {
    $element = Find-UiElementById $Root $AutomationId
    if (-not $element) { throw "Height-check element is missing: $AutomationId" }
    if ($element.Current.IsOffscreen) {
        try { $element.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView() } catch {}
        Start-Sleep -Milliseconds 200
    }
    $scale = [Math]::Max(1.0, [CpcSmokeNative]::GetDpiForWindow($Handle) / 96.0)
    $heightDip = $element.Current.BoundingRectangle.Height / $scale
    if ($heightDip -lt $MinimumDip) {
        throw "Action height can clip its label: $AutomationId ($([Math]::Round($heightDip, 1)) DIP)"
    }
}

function Save-WindowScreenshot([IntPtr]$Handle, [string]$Name) {
    if ($NoScreenshots) { return }
    [CpcSmokeNative]::ShowWindow($Handle, 9) | Out-Null
    [CpcSmokeNative]::SetWindowPos($Handle, [IntPtr]::Zero, 0, 0, 0, 0, 0x0003) | Out-Null
    [CpcSmokeNative]::SetForegroundWindow($Handle) | Out-Null
    Start-Sleep -Milliseconds 200
    New-Item -ItemType Directory -Path $ArtifactsDirectory -Force | Out-Null
    $rectangle = [CpcSmokeNative+RECT]::new()
    if (-not [CpcSmokeNative]::GetWindowRect($Handle, [ref]$rectangle)) { throw 'GetWindowRect failed.' }
    $width = $rectangle.Right - $rectangle.Left
    $height = $rectangle.Bottom - $rectangle.Top
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rectangle.Left, $rectangle.Top, 0, 0, [System.Drawing.Size]::new($width, $height))
        $bitmap.Save((Join-Path $ArtifactsDirectory "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Assert-DarkCompactNavigation([System.Windows.Automation.AutomationElement]$Root, [IntPtr]$Handle) {
    [CpcSmokeNative]::ShowWindow($Handle, 9) | Out-Null
    [CpcSmokeNative]::SetForegroundWindow($Handle) | Out-Null
    Start-Sleep -Milliseconds 200
    foreach ($automationId in @('CompactOverview', 'CompactSettings')) {
        $element = Find-UiElementById $Root $automationId
        if (-not $element -or $element.Current.IsOffscreen) {
            throw "Compact navigation element is unavailable in the narrow layout: $automationId"
        }
        $rectangle = $element.Current.BoundingRectangle
        $width = [Math]::Max(1, [int][Math]::Round($rectangle.Width))
        $height = [Math]::Max(1, [int][Math]::Round($rectangle.Height))
        $bitmap = [System.Drawing.Bitmap]::new($width, $height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen([int][Math]::Round($rectangle.Left), [int][Math]::Round($rectangle.Top),
                                     0, 0, $bitmap.Size)
            $background = $bitmap.GetPixel([Math]::Min(5, $width - 1), [Math]::Min(5, $height - 1))
            if ([Math]::Max($background.R, [Math]::Max($background.G, $background.B)) -gt 160) {
                throw "Compact navigation retained a light background in Dark theme: $automationId"
            }
            $brightPixels = 0
            for ($y = [Math]::Max(0, [int]($height / 4)); $y -lt [Math]::Min($height, [int]($height * 3 / 4)); $y++) {
                for ($x = [Math]::Max(0, [int]($width / 4)); $x -lt [Math]::Min($width, [int]($width * 3 / 4)); $x++) {
                    $pixel = $bitmap.GetPixel($x, $y)
                    if ([Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B)) -gt 150) { $brightPixels++ }
                }
            }
            if ($brightPixels -lt 3) {
                throw "Compact navigation icon is not visible in Dark theme: $automationId"
            }
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}

$process = Start-Process -FilePath $Executable -ArgumentList '--lang','ru' -PassThru
try {
    if (-not (Wait-Until { $process.Refresh(); $process.MainWindowHandle -ne 0 -and $process.Responding } 45)) {
        throw 'Modern did not expose a responsive main window within 45 seconds.'
    }
    $handle = $process.MainWindowHandle
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
    if (-not $root.Current.Name.Contains($expectedVersion)) { throw "Unexpected title: $($root.Current.Name)" }

    [CpcSmokeNative]::SetWindowPos($handle, [IntPtr]::Zero, 80, 60, 1024, 760, 0x0004) | Out-Null
    if (-not (Wait-Until { Select-UiItemById $root 'NavSettings' } 45)) {
        throw 'Settings navigation item did not become available after the initial safe scan.'
    }
    Start-Sleep -Milliseconds 400
    Assert-CardInset $root $handle 'SettingsCard' 'InterfaceSettingsTitle' 'ResetSettingsButton'
    $visibleComboCondition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::ComboBox),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false))
    $settingsCombos = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $visibleComboCondition)
    if ($settingsCombos.Count -lt 3) { throw 'Expected header, language, and theme combo boxes were not found.' }
    $themeCombo = $settingsCombos.Item($settingsCombos.Count - 1)
    $darkLabel = -join ([char[]](0x0422,0x0451,0x043c,0x043d,0x0430,0x044f))
    $lightLabel = -join ([char[]](0x0421,0x0432,0x0435,0x0442,0x043b,0x0430,0x044f))
    $systemLabel = -join ([char[]](0x0421,0x0438,0x0441,0x0442,0x0435,0x043c,0x043d,0x0430,0x044f))
    Select-ComboValue -Combo $themeCombo -ItemName $darkLabel
    Start-Sleep -Milliseconds 400
    Assert-DarkCompactNavigation $root $handle
    Save-WindowScreenshot $handle 'settings-1024x760-dark'
    Select-ComboValue -Combo $themeCombo -ItemName $lightLabel
    Start-Sleep -Milliseconds 400
    Save-WindowScreenshot $handle 'settings-1024x760-light'
    Select-ComboValue -Combo $themeCombo -ItemName $systemLabel
    Start-Sleep -Milliseconds 400
    Save-WindowScreenshot $handle 'settings-1024x760-system'

    if (-not (Select-UiItemById $root 'NavAbout')) {
        throw 'About navigation item is unavailable.'
    }
    Start-Sleep -Milliseconds 300
    Assert-CardInset $root $handle 'AboutCard' 'AboutVersion' 'AboutSupportButton'
    Assert-UiElementMinimumWidth $root $handle 'ShowLocationButton' 190
    Assert-UiElementMinimumWidth $root $handle 'OpenExecutableFolderButton' 190
    Assert-UiElementMinimumWidth $root $handle 'AboutWebsiteButton' 170
    Assert-UiElementMinimumWidth $root $handle 'AboutSupportButton' 170
    Save-WindowScreenshot $handle 'about-1024x760-system'

    if (-not (Select-UiItemById $root 'NavCertificates')) {
        throw 'Public certificates navigation item is unavailable in Russian.'
    }
    Start-Sleep -Milliseconds 300
    Assert-UiElementMinimumHeightById $root $handle 'SelectFilteredCertificatesButton' 36
    Assert-UiElementMinimumHeightById $root $handle 'DeselectFilteredCertificatesButton' 36
    Save-WindowScreenshot $handle 'certificates-1024x760-system-ru'

    $languageCombos = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $visibleComboCondition)
    if ($languageCombos.Count -lt 1) { throw 'Language combo box was not found.' }
    $languageCombo = $languageCombos.Item(0)
    Select-ComboValue -Combo $languageCombo -ItemName 'EN'
    if (-not (Wait-Until { $process.Refresh(); $process.MainWindowTitle -like 'CryptoPro Cleanup Utility*' } 10)) {
        throw 'The title did not switch to English.'
    }
    [CpcSmokeNative]::SetWindowPos($handle, [IntPtr]::Zero, 40, 40, 1280, 720, 0x0004) | Out-Null
    Start-Sleep -Milliseconds 300
    Save-WindowScreenshot $handle 'about-1280x720-system-en'

    $pages = @(
        @{ Name = 'Overview'; Id = 'NavOverview' },
        @{ Name = 'Public certificates'; Id = 'NavCertificates' },
        @{ Name = 'Offline Windows'; Id = 'NavOffline' },
        @{ Name = 'Log and reports'; Id = 'NavReports' },
        @{ Name = 'Settings'; Id = 'NavSettings' },
        @{ Name = 'About'; Id = 'NavAbout' }
    )
    foreach ($page in $pages) {
        $pageName = $page.Name
        if (-not (Select-UiItemById $root $page.Id)) { throw "Navigation item is unavailable: $pageName" }
        Start-Sleep -Milliseconds 250
        if ($pageName -eq 'Public certificates') {
            Assert-UiElementFullyVisible $root 'Deselect filtered'
            Assert-UiElementMinimumHeight $root $handle 'Select filtered' 36
            Assert-UiElementMinimumHeight $root $handle 'Deselect filtered' 36
            $checkboxes = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.AndCondition]::new(
                    [System.Windows.Automation.PropertyCondition]::new(
                        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                        [System.Windows.Automation.ControlType]::CheckBox),
                    [System.Windows.Automation.PropertyCondition]::new(
                        [System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false)))
            foreach ($checkbox in $checkboxes) {
                if ([string]::IsNullOrWhiteSpace($checkbox.Current.Name)) { throw 'A visible checkbox has no accessible name.' }
                $state = $checkbox.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
                if ($state -eq [System.Windows.Automation.ToggleState]::Indeterminate) {
                    throw "Checkbox is indeterminate: $($checkbox.Current.AutomationId)"
                }
            }
            Save-WindowScreenshot $handle 'certificates-1280x720-system-en'
        } elseif ($pageName -eq 'Offline Windows') {
            Assert-UiElementFullyVisible $root 'Safe scan'
            Assert-UiElementFullyVisible $root 'Advanced offline cleanup'
            Save-WindowScreenshot $handle 'offline-1280x720-system-en'
        } elseif ($pageName -eq 'Log and reports') {
            $emptyReportText = 'No reports yet. They appear after saving data or cleanup; a safe scan creates no confidential files.'
            $emptyState = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.PropertyCondition]::new(
                    [System.Windows.Automation.AutomationElement]::NameProperty, $emptyReportText))
            if (-not $emptyState) { throw 'The compact empty report state was not found.' }
            $visibleEdits = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.AndCondition]::new(
                    [System.Windows.Automation.PropertyCondition]::new(
                        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                        [System.Windows.Automation.ControlType]::Edit),
                    [System.Windows.Automation.PropertyCondition]::new(
                        [System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false)))
            $localizedScanLogFound = $false
            foreach ($edit in $visibleEdits) {
                try {
                    $value = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
                    if ($value.StartsWith('Safe scan completed:')) { $localizedScanLogFound = $true }
                } catch {}
            }
            if (-not $localizedScanLogFound) {
                $visibleTexts = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.AndCondition]::new(
                        [System.Windows.Automation.PropertyCondition]::new(
                            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                            [System.Windows.Automation.ControlType]::Text),
                        [System.Windows.Automation.PropertyCondition]::new(
                            [System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false)))
                foreach ($textElement in $visibleTexts) {
                    if ($textElement.Current.Name -like '*Safe scan completed:*') { $localizedScanLogFound = $true; break }
                }
            }
            if (-not $localizedScanLogFound) { throw 'The safe-scan log did not switch to English.' }
            Save-WindowScreenshot $handle 'reports-empty-1280x720-system-en'
        } elseif ($pageName -eq 'Settings') {
            Assert-UiElementFullyVisible $root 'Reset interface settings'
        } elseif ($pageName -eq 'About') {
            Assert-UiElementFullyVisible $root 'Show location'
            Assert-UiElementFullyVisible $root 'Open program folder'
        } elseif ($pageName -eq 'Overview') {
            Assert-UiElementFullyVisible $root 'Scan again'
        }
    }

    Write-Host "UI smoke passed: launch, responsive resize, Dark/Light/System, all six pages, empty reports, checkbox accessibility, Settings, About, and RU/EN title. Artifacts: $ArtifactsDirectory"
} finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id -Force }
    }
    if (-not $settingsExisted) {
        if (Test-Path -LiteralPath $settingsPath) { Remove-Item -LiteralPath $settingsPath -Force }
    } else {
        $settingsKey = Get-Item -LiteralPath $settingsPath
        foreach ($name in $settingsKey.GetValueNames()) {
            if (-not $savedSettings.ContainsKey($name)) { Remove-ItemProperty -LiteralPath $settingsPath -Name $name -Force }
        }
        foreach ($entry in $savedSettings.GetEnumerator()) {
            New-ItemProperty -LiteralPath $settingsPath -Name $entry.Key -Value $entry.Value[0] -PropertyType $entry.Value[1] -Force | Out-Null
        }
    }
}
