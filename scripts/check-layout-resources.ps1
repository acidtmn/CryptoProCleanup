param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$appPath = Join-Path $projectRoot 'App.xaml'
$windowPath = Join-Path $projectRoot 'MainWindow.xaml'
$app = Get-Content -LiteralPath $appPath -Raw
$window = Get-Content -LiteralPath $windowPath -Raw

$spacingTokens = @(
    'Space4','Space8','Space12','Space16','Space20','Space24','Space32',
    'CardPadding','CompactCardPadding','StatCardPadding','CalloutPadding',
    'DialogContentPadding','TableHeaderPadding','TableRowPadding','TableFooterPadding'
)
$cardStyles = @('CardChromeStyle','ContentCardStyle','CompactContentCardStyle','StatCardStyle',
                'TableCardStyle','CalloutCardStyle','NavigationSafetyCardStyle')
foreach ($key in $spacingTokens + $cardStyles) {
    if ($app -notmatch ('x:Key="' + [regex]::Escape($key) + '"')) {
        throw "Required layout resource is missing: $key"
    }
}

if ($window -match 'StaticResource\s+CardBorderStyle') {
    throw 'MainWindow.xaml still uses the compatibility-only CardBorderStyle.'
}
if ($window -match 'StaticResource\s+CardChromeStyle') {
    throw 'Authored UI must use a semantic card style instead of bare CardChromeStyle.'
}
if ($window -match 'Margin="[^"]*-[0-9]') {
    throw 'A negative authored margin was found in MainWindow.xaml.'
}

# Padding=0 is intentional only for the compact navigation icon buttons. Table
# containers receive zero padding from TableCardStyle and own their inner inset.
$zeroPaddingNames = [regex]::Matches($window, '<[^>]+x:Name="([^"]+)"[^>]+Padding="0"[^>]*>') |
    ForEach-Object { $_.Groups[1].Value }
$zeroPaddingWhitelist = @('CompactOverview','CompactCertificates','CompactOffline',
                          'CompactReports','CompactSettings','CompactAbout')
$unexpectedZeroPadding = @($zeroPaddingNames | Where-Object { $_ -notin $zeroPaddingWhitelist })
if ($unexpectedZeroPadding.Count) {
    throw "Unexpected authored Padding=0: $($unexpectedZeroPadding -join ', ')"
}

$knownLongActions = @(
    'DeselectFilteredCertificatesButton','RescanButton','ScanOfflineButton','ShowLocationButton',
    'OpenExecutableFolderButton','ResetSettingsButton','CleanOfflineButton'
)
foreach ($name in $knownLongActions) {
    $match = [regex]::Match($window, '<(?:Button|HyperlinkButton)[^>]*x:Name="' + [regex]::Escape($name) + '"[^>]*>')
    if (-not $match.Success) { throw "Known adaptive action is missing: $name" }
    if ($match.Value -match '\sWidth="[0-9]') { throw "Known long action has a fixed width: $name" }
}

[xml]$null = Get-Content -LiteralPath $appPath -Raw
[xml]$null = Get-Content -LiteralPath $windowPath -Raw
Write-Host "Layout resource check passed: $($spacingTokens.Count) tokens, $($cardStyles.Count) semantic card styles, documented zero-padding whitelist, and adaptive long actions."
