param(
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$searchRoot = [IO.Path]::GetFullPath($projectRoot)
$toolsPath = $null
for ($i = 0; $i -lt 8; $i++) {
    $candidate = Join-Path $searchRoot 'tools.local.json'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $toolsPath = $candidate
        break
    }
    $parent = Split-Path -Parent $searchRoot
    if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $searchRoot) { break }
    $searchRoot = $parent
}
$as3Root = Join-Path $projectRoot 'as3'
$templatePath = Join-Path $PSScriptRoot 'publish-ammo-wheel.jsfl'
$runtimeSwf = Join-Path $projectRoot 'data\Interface\MWB_AmmoSwitchWheel.swf'
$runId = Get-Date -Format 'yyyyMMdd-HHmmssfff'
$outputRoot = Join-Path $projectRoot "build\as3\ammo-wheel\$runId"
$flaPath = Join-Path $outputRoot 'MWBAmmoSwitchWheel.fla'
$publishedSwf = Join-Path $outputRoot 'MWBAmmoSwitchWheel.swf'
$generatedScript = Join-Path $outputRoot 'publish-ammo-wheel.generated.jsfl'
$resultLog = Join-Path $outputRoot 'publish-result.txt'
$outputLog = Join-Path $outputRoot 'animate-output.txt'

function ConvertTo-FileUri([string]$Path) {
    return ([System.Uri]([System.IO.Path]::GetFullPath($Path))).AbsoluteUri
}

if (-not $toolsPath -or -not (Test-Path -LiteralPath $toolsPath -PathType Leaf)) {
    throw 'Could not find tools.local.json while walking up from the project root.'
}
if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $as3Root 'MWBAmmoSwitchWheel.as') -PathType Leaf)) {
    throw 'Missing the wheel AS3 source or JSFL publisher.'
}

$tools = Get-Content -LiteralPath $toolsPath -Raw | ConvertFrom-Json
$animatePath = [string]$tools.animate2024
if (-not $animatePath -or -not (Test-Path -LiteralPath $animatePath -PathType Leaf)) {
    throw 'Adobe Animate 2024 was not found in tools.local.json.'
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$jsfl = Get-Content -LiteralPath $templatePath -Raw
$jsfl = $jsfl.Replace('__AS3_PATH__', $as3Root.Replace('\', '/'))
$jsfl = $jsfl.Replace('__FLA_URI__', (ConvertTo-FileUri $flaPath))
$jsfl = $jsfl.Replace('__LOG_URI__', (ConvertTo-FileUri $resultLog))
$jsfl = $jsfl.Replace('__OUTPUT_URI__', (ConvertTo-FileUri $outputLog))
Set-Content -LiteralPath $generatedScript -Value $jsfl -Encoding UTF8

$startedAt = Get-Date
Start-Process -FilePath $animatePath -ArgumentList @('"' + $generatedScript + '"', '-AlwaysRunJSFL') -WindowStyle Hidden | Out-Null
$deadline = $startedAt.AddSeconds($TimeoutSeconds)
while (-not (Test-Path -LiteralPath $resultLog -PathType Leaf)) {
    if ((Get-Date) -ge $deadline) {
        throw "Timed out waiting for Animate to publish the wheel. Output: $outputRoot"
    }
    Start-Sleep -Milliseconds 250
}

$result = Get-Content -LiteralPath $resultLog
if ($result.Count -lt 1 -or $result[0] -ne 'OK') {
    $details = if (Test-Path -LiteralPath $outputLog -PathType Leaf) { Get-Content -LiteralPath $outputLog -Raw } else { 'Animate produced no output log.' }
    throw "Animate publish failed: $($result -join ' '); $details"
}
if (-not (Test-Path -LiteralPath $publishedSwf -PathType Leaf) -or
    (Get-Item -LiteralPath $publishedSwf).LastWriteTime -lt $startedAt.AddSeconds(-2)) {
    throw "Animate reported success without producing a fresh SWF: $publishedSwf"
}

New-Item -ItemType Directory -Path (Split-Path -Parent $runtimeSwf) -Force | Out-Null
Copy-Item -LiteralPath $publishedSwf -Destination $runtimeSwf -Force
$hash = (Get-FileHash -LiteralPath $runtimeSwf -Algorithm SHA256).Hash
Write-Host "Published: $publishedSwf"
Write-Host "Staged:    $runtimeSwf"
Write-Host "Size:      $((Get-Item -LiteralPath $runtimeSwf).Length) bytes"
Write-Host "SHA-256:   $hash"
