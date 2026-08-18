param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [uri]$SourceUrl,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$ExpectedSha256,

    [Parameter(Mandatory = $true)]
    [string]$Destination,

    [Parameter(Mandatory = $true)]
    [string]$ArchivePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$requiredFiles = @(
    'filter2.h',
    'plugin2.h',
    'aviutl2_plugin_sdk.txt',
    'license.txt',
    'MediaFilter.cpp',
    'MediaObject.cpp'
)

function Test-SdkRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    foreach ($relativePath in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relativePath) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

$archiveFullPath = [System.IO.Path]::GetFullPath($ArchivePath)
$destinationFullPath = [System.IO.Path]::GetFullPath($Destination)
$archiveDirectory = Split-Path -Parent $archiveFullPath
$extractDirectory = Join-Path $archiveDirectory 'aviutl2-sdk-extracted'

New-Item -ItemType Directory -Force -Path $archiveDirectory | Out-Null
Remove-Item -LiteralPath $archiveFullPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $extractDirectory -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "SDK version: $Version"
Write-Host "Download source: $SourceUrl"
Write-Host "Downloading SDK archive to temporary file: $archiveFullPath"

try {
    Invoke-WebRequest -Uri $SourceUrl -OutFile $archiveFullPath
} catch {
    throw "Failed to download the official AviUtl2 SDK from '$SourceUrl': $($_.Exception.Message)"
}

$actualSha256 = (Get-FileHash -LiteralPath $archiveFullPath -Algorithm SHA256).Hash.ToUpperInvariant()
$expectedSha256Upper = $ExpectedSha256.ToUpperInvariant()
Write-Host "Archive SHA-256: $actualSha256"
if ($actualSha256 -ne $expectedSha256Upper) {
    # Keep this script ASCII-compatible so Windows PowerShell 5.1 can parse it
    # even when the repository stores it as UTF-8 without a BOM.
    $sdkUpdatedHint = -join @(
        0x516C, 0x5F0F, 0x0053, 0x0044, 0x004B, 0x304C, 0x66F4,
        0x65B0, 0x3055, 0x308C, 0x305F, 0x53EF, 0x80FD, 0x6027,
        0x304C, 0x3042, 0x308A, 0x307E, 0x3059, 0x3002 |
            ForEach-Object { [char]$_ }
    )
    $mismatchMessage = @(
        'AviUtl2 SDK SHA-256 mismatch.'
        "Pinned SDK version: $Version"
        "Expected SHA-256: $expectedSha256Upper"
        "Actual SHA-256: $actualSha256"
        "Official URL: $SourceUrl"
        "$sdkUpdatedHint Verify the official distribution and update the pin manually."
    ) -join [System.Environment]::NewLine
    throw $mismatchMessage
}

New-Item -ItemType Directory -Force -Path $extractDirectory | Out-Null
try {
    Expand-Archive -LiteralPath $archiveFullPath -DestinationPath $extractDirectory -Force
} catch {
    throw "Failed to extract the AviUtl2 SDK archive: $($_.Exception.Message)"
}

$rootCandidates = @($extractDirectory)
$rootCandidates += @(Get-ChildItem -LiteralPath $extractDirectory -Directory | ForEach-Object { $_.FullName })
$validRoots = @($rootCandidates | Where-Object { Test-SdkRoot -Path $_ })
if ($validRoots.Count -ne 1) {
    $candidateText = if ($validRoots.Count -eq 0) { 'none' } else { $validRoots -join ', ' }
    throw "Unexpected AviUtl2 SDK archive structure. Valid SDK roots: $candidateText"
}
$sdkRoot = $validRoots[0]

$workspaceRoot = [System.IO.Path]::GetFullPath((Get-Location).Path)
$workspacePrefix = $workspaceRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
    [System.IO.Path]::DirectorySeparatorChar
if (-not $destinationFullPath.StartsWith(
        $workspacePrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace an SDK destination outside the workspace: '$destinationFullPath'"
}

Remove-Item -LiteralPath $destinationFullPath -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $destinationFullPath | Out-Null
Copy-Item -Path (Join-Path $sdkRoot '*') -Destination $destinationFullPath -Recurse -Force

Write-Host "Extraction destination: $destinationFullPath"
foreach ($relativePath in $requiredFiles) {
    $requiredPath = Join-Path $destinationFullPath $relativePath
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required AviUtl2 SDK file is missing after extraction: '$requiredPath'"
    }
    Write-Host "Required file: $relativePath [OK]"
}
Write-Host 'Required files validation result: success'
