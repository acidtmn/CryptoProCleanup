param(
    [string]$Executable,
    [string]$ArtifactsDirectory,
    [switch]$NoScreenshots,
    [switch]$ExerciseBackup,
    [switch]$PublicGallery
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
if ($PublicGallery -and ($NoScreenshots -or $ExerciseBackup)) {
    throw 'PublicGallery cannot be combined with NoScreenshots or ExerciseBackup.'
}

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
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CpcSmokeNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, uint attribute, out RECT r, uint size);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder text, int count);
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
    foreach ($pattern in @([System.Windows.Automation.InvokePattern]::Pattern,
                           [System.Windows.Automation.SelectionItemPattern]::Pattern)) {
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
    foreach ($pattern in @([System.Windows.Automation.InvokePattern]::Pattern,
                           [System.Windows.Automation.SelectionItemPattern]::Pattern)) {
        try {
            $implementation = $Element.GetCurrentPattern($pattern)
            if ($pattern -eq [System.Windows.Automation.SelectionItemPattern]::Pattern) { $implementation.Select() }
            else { $implementation.Invoke() }
            return $true
        } catch {}
    }
    return $false
}

function Invoke-ModalAction([IntPtr]$Handle, [string]$AutomationId) {
    Start-Job -ArgumentList $Handle.ToInt64(), $AutomationId -ScriptBlock {
        param($NativeWindow, $ButtonId)
        Add-Type -AssemblyName UIAutomationClient
        Add-Type -AssemblyName UIAutomationTypes
        $window = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$NativeWindow)
        $button = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.PropertyCondition]::new(
                [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $ButtonId))
        $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    }
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
    $visibleRectangle = [CpcSmokeNative+RECT]::new()
    if ([CpcSmokeNative]::DwmGetWindowAttribute($Handle, 9, [ref]$visibleRectangle, 16) -eq 0) {
        $rectangle = $visibleRectangle
    }
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
$pickerInvokeJobs = @()
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
    if ($PublicGallery) {
        # Перед публичным захватом убираем путь и скрываем личные сертификаты штатным фильтром.
        [CpcSmokeNative]::SetWindowPos($handle, [IntPtr]::Zero, 40, 40, 1440, 1000, 0x0004) | Out-Null
        if (-not (Select-UiItemById $root 'NavCleanup')) { throw 'Removal is unavailable.' }
        if (-not (Wait-Until { $null -ne (Find-UiElementById $root 'BackupPath') } 10)) { throw 'Backup path is unavailable.' }
        $backupInput = Find-UiElementById $root 'BackupPath'
        $backupInput.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('')
        if (-not (Select-UiItemById $root 'NavCertificates')) { throw 'Certificates are unavailable.' }
        if (-not (Wait-Until { $null -ne (Find-UiElementById $root 'CertificateSearch') } 10)) { throw 'Certificate search is unavailable.' }
        $searchInput = Find-UiElementById $root 'CertificateSearch'
        $searchInput.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('github-gallery')
        Start-Sleep -Milliseconds 400
        $initials = Find-UiElementById $root 'SelectedCertInitials'
        $thumbprint = Find-UiElementById $root 'SelectedCertThumbprint'
        if ($initials.Current.Name -ne [string][char]0x2014 -or
            $thumbprint.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value -ne ('SHA-1: ' + [char]0x2014)) {
            throw 'The certificate filter did not hide private details. Public capture aborted.'
        }
        $galleryPages = @(
            @{ Id = 'NavOverview'; File = '01-home' },
            @{ Id = 'NavCleanup'; File = '02-removal' },
            @{ Id = 'NavCertificates'; File = '03-certificates' },
            @{ Id = 'NavOffline'; File = '04-offline' },
            @{ Id = 'NavReports'; File = '05-reports' },
            @{ Id = 'NavSettings'; File = '06-settings' },
            @{ Id = 'NavAbout'; File = '07-about' }
        )
        foreach ($page in $galleryPages) {
            if (-not (Select-UiItemById $root $page.Id)) { throw "Gallery navigation failed: $($page.Id)" }
            Start-Sleep -Milliseconds 400
            $visible = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.PropertyCondition]::new(
                    [System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false))
            foreach ($element in $visible) {
                $text = $element.Current.Name
                if ($text -match '(?i)[a-z]:\\(?:users|kirill)\\|S-1-5-21-|\b[0-9a-f]{40}\b') {
                    throw "A personal path, SID or thumbprint is visible. Public capture aborted: $($page.Id)"
                }
            }
            Save-WindowScreenshot $handle $page.File
        }
        if (-not (Wait-Until { Select-UiItemById $root 'NavSettings' } 10)) { throw 'Settings is unavailable.' }
        Start-Sleep -Milliseconds 400
        Select-ComboValue -Combo $themeCombo -ItemName $lightLabel
        if (-not (Wait-Until { Select-UiItemById $root 'NavOverview' } 10)) { throw 'Home is unavailable.' }
        Start-Sleep -Milliseconds 400
        Save-WindowScreenshot $handle '08-home-light'
        Write-Host "Public gallery captured: seven sections plus Light home. Review every image before upload: $ArtifactsDirectory"
        return
    }
    Assert-DarkCompactNavigation $root $handle
    Save-WindowScreenshot $handle 'settings-1024x760-dark'
    if (-not (Select-UiItemById $root 'NavOverview')) { throw 'Home is unavailable in Dark theme.' }
    Start-Sleep -Milliseconds 300
    Save-WindowScreenshot $handle 'home-1024x760-dark-ru'
    if (-not (Select-UiItemById $root 'NavSettings')) { throw 'Settings is unavailable.' }
    Select-ComboValue -Combo $themeCombo -ItemName $lightLabel
    Start-Sleep -Milliseconds 400
    Save-WindowScreenshot $handle 'settings-1024x760-light'
    Select-ComboValue -Combo $themeCombo -ItemName $systemLabel
    Start-Sleep -Milliseconds 400
    Save-WindowScreenshot $handle 'settings-1024x760-system'

    if (-not (Select-UiItemById $root 'NavOverview')) { throw 'Home is unavailable.' }
    Start-Sleep -Milliseconds 300
    foreach ($id in @('SaveDataButton','GoCleanupButton','GoOfflineButton','HomeTasksTitle')) {
        $element = Find-UiElementById $root $id
        if (-not $element -or $element.Current.IsOffscreen) { throw "Guided home action is not visible: $id" }
    }
    Save-WindowScreenshot $handle 'home-1024x760-system-ru'
    if (-not (Select-UiItemById $root 'NavCleanup')) { throw 'Removal page is unavailable.' }
    Start-Sleep -Milliseconds 300
    foreach ($id in @('NextStepButton','NextStepTitle')) {
        $element = Find-UiElementById $root $id
        if (-not $element -or $element.Current.IsOffscreen) { throw "Removal next step is not visible: $id" }
    }
    Save-WindowScreenshot $handle 'cleanup-1024x760-system-ru'
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
        @{ Name = 'Removal'; Id = 'NavCleanup' },
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
            Assert-UiElementFullyVisible $root 'Export selected'
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
            Assert-UiElementFullyVisible $root 'Back up data'
            Assert-UiElementFullyVisible $root 'Remove applications'
            Assert-UiElementFullyVisible $root 'Another drive'
            Save-WindowScreenshot $handle 'home-1280x720-system-en'
        } elseif ($pageName -eq 'Removal') {
            $next = Find-UiElementById $root 'NextStepTitle'
            if (-not $next -or $next.Current.IsOffscreen -or [string]::IsNullOrWhiteSpace($next.Current.Name)) {
                throw 'The persistent next-step explanation is missing.'
            }
            Save-WindowScreenshot $handle 'cleanup-1280x720-system-en'
        }
    }

    if (-not (Select-UiItemById $root 'NavOverview')) { throw 'Home is unavailable for shortcut checks.' }
    [CpcSmokeNative]::SetForegroundWindow($handle) | Out-Null
    Start-Sleep -Milliseconds 300
    (Find-UiElementById $root 'HomePage').GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern).SetScrollPercent(-1, 100)
    Start-Sleep -Milliseconds 300
    $certificateShortcut = Find-UiElementById $root 'GoCertificatesButton'
    $certificateShortcut.SetFocus()
    Start-Sleep -Milliseconds 200
    $certificateShortcut.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    if (-not (Wait-Until { (Find-UiElementById $root 'PageTitle').Current.Name -eq 'Public certificates' } 5)) {
        Save-WindowScreenshot $handle 'shortcut-diagnostic'
        throw "Certificate shortcut did not navigate: title=$((Find-UiElementById $root 'PageTitle').Current.Name)"
    }
    if (-not (Select-UiItemById $root 'NavOverview')) { throw 'Home did not reopen.' }
    Start-Sleep -Milliseconds 300
    (Find-UiElementById $root 'HomePage').GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern).SetScrollPercent(-1, 0)
    Start-Sleep -Milliseconds 300
    $offlineShortcut = Find-UiElementById $root 'GoOfflineButton'
    $offlineShortcut.SetFocus()
    Start-Sleep -Milliseconds 200
    $offlineShortcut.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    if (-not (Wait-Until { (Find-UiElementById $root 'PageTitle').Current.Name -eq 'Another drive' } 5)) {
        Save-WindowScreenshot $handle 'other-drive-shortcut-diagnostic'
        throw "Another-drive shortcut did not navigate: title=$((Find-UiElementById $root 'PageTitle').Current.Name)"
    }
    Select-UiItemById $root 'NavOverview' | Out-Null
    Start-Sleep -Milliseconds 300
    (Find-UiElementById $root 'HomePage').GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern).SetScrollPercent(-1, 0)
    (Find-UiElementById $root 'GoCleanupButton').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    if (-not (Wait-Until { (Find-UiElementById $root 'PageTitle').Current.Name -eq 'Remove applications' } 5)) {
        throw 'The removal task card did not open the removal page.'
    }
    [CpcSmokeNative]::SetWindowPos($handle, [IntPtr]::Zero, 40, 40, 900, 650, 0x0004) | Out-Null
    Select-UiItemById $root 'NavOverview' | Out-Null
    Start-Sleep -Milliseconds 400
    Save-WindowScreenshot $handle 'home-900x650-en'
    Select-UiItemById $root 'NavCleanup' | Out-Null
    Start-Sleep -Milliseconds 300
    $windowBounds = $root.Current.BoundingRectangle
    foreach ($fixedId in @('CompactAbout','FooterStatus')) {
        $fixed = Find-UiElementById $root $fixedId
        if (-not $fixed -or $fixed.Current.IsOffscreen -or $fixed.Current.BoundingRectangle.Bottom -gt $windowBounds.Bottom) {
            throw "The minimum window clips a fixed element: $fixedId"
        }
    }
    $nextAction = Find-UiElementById $root 'NextStepButton'
    $nextBounds = $nextAction.Current.BoundingRectangle
    if ($nextAction.Current.IsOffscreen -or $nextBounds.Bottom -gt $windowBounds.Bottom -or $nextBounds.Right -gt $windowBounds.Right) {
        Save-WindowScreenshot $handle 'cleanup-900x650-diagnostic'
        throw 'The fixed removal action is clipped in the minimum window.'
    }
    Save-WindowScreenshot $handle 'cleanup-900x650-en'
    Select-UiItemById $root 'NavCertificates' | Out-Null
    Start-Sleep -Milliseconds 300
    Assert-UiElementFullyVisible $root 'Export selected'
    Save-WindowScreenshot $handle 'certificates-900x650-en'
    if ($ExerciseBackup) {
        [CpcSmokeNative]::SetWindowPos($handle, [IntPtr]::Zero, 40, 40, 1280, 800, 0x0004) | Out-Null
        Select-UiItemById $root 'NavOverview' | Out-Null
        Start-Sleep -Milliseconds 300
        (Find-UiElementById $root 'HomePage').GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern).SetScrollPercent(-1, 0)
        Start-Sleep -Milliseconds 300
        $pickerInvokeJobs += Invoke-ModalAction $handle 'SaveDataButton'
        if (-not (Wait-Until {
            $popupHandle = [CpcSmokeNative]::GetLastActivePopup($handle)
            $popupHandle -ne $handle -and $popupHandle -ne [IntPtr]::Zero
        } 15)) {
            throw 'The backup picker did not expose its owned window.'
        }
        $pickerClass = [Text.StringBuilder]::new(256)
        [CpcSmokeNative]::GetClassNameW([CpcSmokeNative]::GetLastActivePopup($handle), $pickerClass, 256) | Out-Null
        if ($pickerClass.ToString() -ne '#32770') { throw 'The owned window is not the expected folder picker.' }
        Start-Sleep -Milliseconds 500
        [CpcSmokeNative]::PostMessageW([CpcSmokeNative]::GetLastActivePopup($handle), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not (Wait-Until { (Find-UiElementById $root 'SaveDataButton').Current.IsEnabled } 10)) { throw 'The backup picker did not release its operation.' }

        Select-UiItemById $root 'NavCertificates' | Out-Null
        Start-Sleep -Milliseconds 300
        (Find-UiElementById $root 'DeselectFilteredCertificatesButton').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        Start-Sleep -Milliseconds 300
        $firstCertificate = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.AndCondition]::new(
                [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::CheckBox),
                [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::IsOffscreenProperty, $false)))
        if (-not $firstCertificate) { throw 'A public certificate is required for this optional backup scenario.' }
        $firstCertificate.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
        if (-not (Wait-Until { (Find-UiElementById $root 'CertificateSelectedCount').Current.Name -match '^Selected: 1 ' } 5)) {
            throw 'The certificate selection was not applied.'
        }
        $backupTestRoot = Join-Path ([IO.Path]::GetTempPath()) ('CpcUiBackup-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $backupTestRoot | Out-Null
        Select-UiItemById $root 'NavOverview' | Out-Null
        Start-Sleep -Milliseconds 300
        (Find-UiElementById $root 'HomePage').GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern).SetScrollPercent(-1, 0)
        $pickerInvokeJobs += Invoke-ModalAction $handle 'SaveDataButton'
        if (-not (Wait-Until {
            $popup = [CpcSmokeNative]::GetLastActivePopup($handle)
            $popup -ne $handle -and $popup -ne [IntPtr]::Zero -and [CpcSmokeNative]::GetDlgItem($popup, 1152) -ne [IntPtr]::Zero
        } 15)) { throw 'The backup folder-name input is unavailable.' }
        $popup = [CpcSmokeNative]::GetLastActivePopup($handle)
        [CpcSmokeNative]::SetForegroundWindow($popup) | Out-Null
        Start-Sleep -Milliseconds 300
        if ([CpcSmokeNative]::GetForegroundWindow() -ne $popup) { throw 'The backup picker is not the active window.' }
        [System.Windows.Forms.SendKeys]::SendWait('^l')
        [System.Windows.Forms.SendKeys]::SendWait($backupTestRoot)
        [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
        Start-Sleep -Milliseconds 1000
        $chooseFolder = [CpcSmokeNative]::GetDlgItem($popup, 1)
        [CpcSmokeNative]::PostMessageW($chooseFolder, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not (Wait-Until { @(Get-ChildItem -LiteralPath $backupTestRoot -Recurse -File -Filter 'cleanup.log').Count -eq 1 } 45)) {
            Save-WindowScreenshot $handle 'backup-save-diagnostic'
            throw 'The GUI backup did not create its output files.'
        }
        foreach ($required in @('licenses.txt','report.json','summary.txt','cleanup.log')) {
            if (@(Get-ChildItem -LiteralPath $backupTestRoot -Recurse -File -Filter $required).Count -ne 1) {
                throw "The GUI backup is missing its output: $required"
            }
        }
        if (@(Get-ChildItem -LiteralPath $backupTestRoot -Recurse -File -Filter '*.cer').Count -ne 1) {
            throw 'The GUI backup did not preserve the one-certificate selection.'
        }
        $closeMessage = Find-UiElementById $root 'CloseButton'
        if ($closeMessage -and $closeMessage.Current.IsEnabled) {
            $closeMessage.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        }
        if (-not (Wait-Until { (Find-UiElementById $root 'PageTitle').Current.Name -eq 'Log and reports' } 10)) {
            throw 'The completed backup did not navigate to Reports.'
        }
        Write-Host 'GUI backup passed: picker cancellation, a real folder selection, one selected public certificate, licenses, JSON, text report, and log.'
    }
    Write-Host "UI smoke passed: launch, responsive resize, Dark/Light/System, all seven pages, empty reports, checkbox accessibility, RU/EN, and home shortcuts. Artifacts: $ArtifactsDirectory"
} finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(15000)) { Stop-Process -Id $process.Id -Force }
    }
    foreach ($job in $pickerInvokeJobs) {
        if ($job.State -eq 'Running') { Stop-Job $job }
        Remove-Job $job
    }
    if ($backupTestRoot -and (Test-Path -LiteralPath $backupTestRoot)) {
        $resolvedTest = [IO.Path]::GetFullPath($backupTestRoot)
        $expectedPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\CpcUiBackup-'
        if (-not $resolvedTest.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe backup-test cleanup path.' }
        Remove-Item -LiteralPath $resolvedTest -Recurse -Force
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
