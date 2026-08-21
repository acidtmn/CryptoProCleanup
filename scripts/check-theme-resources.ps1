param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$appXamlPath = Join-Path $projectRoot 'App.xaml'
$windowXamlPath = Join-Path $projectRoot 'MainWindow.xaml'
$windowCppPath = Join-Path $projectRoot 'MainWindow.xaml.cpp'

[xml]$document = Get-Content -LiteralPath $appXamlPath -Raw
$namespaces = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
$namespaces.AddNamespace('p', 'http://schemas.microsoft.com/winfx/2006/xaml/presentation')
$namespaces.AddNamespace('x', 'http://schemas.microsoft.com/winfx/2006/xaml')

$required = @(
    'AppBackgroundBrush','NavigationBackgroundBrush','SurfaceBrush','SurfaceRaisedBrush',
    'SurfaceHoverBrush','SurfacePressedBrush','SurfaceSelectedBrush','SurfaceDisabledBrush',
    'SubtleBorderBrush','StrongBorderBrush','DividerBrush','PrimaryTextBrush','SecondaryTextBrush',
    'TertiaryTextBrush','DisabledTextBrush','InverseTextBrush','AccentBrush','AccentHoverBrush',
    'AccentPressedBrush','AccentSubtleBrush','AccentBorderBrush','SuccessBrush','SuccessSurfaceBrush',
    'SuccessBorderBrush','WarningBrush','WarningSurfaceBrush','WarningBorderBrush','DangerBrush',
    'DangerHoverBrush','DangerPressedBrush','DangerSurfaceBrush','DangerBorderBrush',
    'PrimaryButtonBackgroundBrush','PrimaryButtonForegroundBrush','PrimaryButtonHoverBrush',
    'PrimaryButtonPressedBrush','PrimaryButtonDisabledBrush','SecondaryButtonBackgroundBrush',
    'SecondaryButtonForegroundBrush','SecondaryButtonHoverBrush','SecondaryButtonPressedBrush',
    'InputBackgroundBrush','InputForegroundBrush','InputBorderBrush','InputPlaceholderBrush',
    'BadgeNeutralBackgroundBrush','BadgeNeutralForegroundBrush','BadgeSuccessBackgroundBrush',
    'BadgeSuccessForegroundBrush','BadgeWarningBackgroundBrush','BadgeWarningForegroundBrush',
    'BadgeDangerBackgroundBrush','BadgeDangerForegroundBrush','SelectionIndicatorBrush','FocusBrush',
    'OverlayBrush','DialogBackgroundBrush','DialogForegroundBrush'
)

$themeKeys = @{}
foreach ($theme in @('Dark', 'Light', 'HighContrast')) {
    $node = $document.SelectSingleNode("//p:ResourceDictionary.ThemeDictionaries/p:ResourceDictionary[@x:Key='$theme']", $namespaces)
    if (-not $node) { throw "Theme dictionary '$theme' is missing." }
    $keys = @($node.ChildNodes | ForEach-Object { $_.GetAttribute('Key', 'http://schemas.microsoft.com/winfx/2006/xaml') } |
        Where-Object { $_ } | Sort-Object -Unique)
    $themeKeys[$theme] = $keys
    $missing = @($required | Where-Object { $_ -notin $keys })
    if ($missing.Count) { throw "Theme '$theme' is missing: $($missing -join ', ')" }
}

$baseline = $themeKeys['Dark']
foreach ($theme in @('Light', 'HighContrast')) {
    $missing = @($baseline | Where-Object { $_ -notin $themeKeys[$theme] })
    $extra = @($themeKeys[$theme] | Where-Object { $_ -notin $baseline })
    if ($missing.Count -or $extra.Count) {
        throw "Theme '$theme' differs from Dark. Missing: $($missing -join ', '); extra: $($extra -join ', ')"
    }
}

$windowXaml = Get-Content -LiteralPath $windowXamlPath -Raw
$windowCpp = Get-Content -LiteralPath $windowCppPath -Raw
$references = @()
$references += [regex]::Matches($windowXaml, '\{ThemeResource\s+([A-Za-z0-9_]+)\}') | ForEach-Object { $_.Groups[1].Value }
$references += [regex]::Matches($windowCpp, 'ThemeBrush\(L"([A-Za-z0-9_]+)"\)') | ForEach-Object { $_.Groups[1].Value }
$unknown = @($references | Sort-Object -Unique | Where-Object { $_ -notin $baseline })
if ($unknown.Count) { throw "Unknown theme resources are referenced: $($unknown -join ', ')" }

if ($windowXaml -match '#[0-9A-Fa-f]{6,8}') { throw 'Hard-coded hexadecimal colors were found in MainWindow.xaml.' }
if ($windowCpp -match 'ColorHelper::FromArgb|SolidColorBrush\s*\(') { throw 'Hard-coded runtime brushes were found in MainWindow.xaml.cpp.' }
if ($windowXaml -match '\{StaticResource\s+[^}]*Brush[^}]*\}') { throw 'A brush is referenced through StaticResource in MainWindow.xaml.' }
if ($windowXaml -match 'RequestedTheme\s*=\s*"Dark"') { throw 'MainWindow forces the Dark theme.' }
if ((Get-Content -LiteralPath $appXamlPath -Raw) -match '<Style\s+TargetType="TextBlock"') {
    throw 'An implicit TextBlock style can override Button.Foreground and is not allowed.'
}
if ($windowXaml -match '<Button[^>]*>[\s\S]{0,500}?<TextBlock[^>]*Foreground="\{ThemeResource\s+PrimaryTextBrush\}"') {
    throw 'A button child forces PrimaryTextBrush instead of inheriting Button.Foreground.'
}

Write-Host "Theme resource check passed: $($baseline.Count) identical keys in Dark, Light, and HighContrast; $(@($references | Sort-Object -Unique).Count) referenced keys resolved."
