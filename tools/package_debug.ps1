param(
    [string]$BuildDirectory = "",
    [string]$DestinationRoot = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "..")
)
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot `
        "out\build\vscode-windows-app\Debug"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $DestinationRoot = [Environment]::GetFolderPath("Desktop")
}
$DestinationRoot = [System.IO.Path]::GetFullPath($DestinationRoot)

$requiredFiles = @(
    "xpbd_baker_app.exe",
    "SDL3.dll",
    "fmtd.dll",
    "spdlogd.dll",
    "vulkan-1.dll",
    "sl.interposer.dll",
    "sl.common.dll",
    "sl.dlss.dll",
    "sl.dlss_d.dll",
    "sl.dlss_g.dll",
    "sl.pcl.dll",
    "sl.reflex.dll",
    "NvLowLatencyVk.dll",
    "nvngx_dlss.dll",
    "nvngx_dlssd.dll",
    "nvngx_dlssg.dll"
)

foreach ($name in $requiredFiles) {
    $source = Join-Path $BuildDirectory $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required package file is missing: $source"
    }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$destination = Join-Path $DestinationRoot "XPBD_Baker_RT_Debug-$stamp"
if (Test-Path -LiteralPath $destination) {
    throw "Package destination already exists: $destination"
}

New-Item -Path $destination -ItemType Directory | Out-Null
foreach ($name in $requiredFiles) {
    Copy-Item -LiteralPath (Join-Path $BuildDirectory $name) `
        -Destination (Join-Path $destination $name)
}

foreach ($directoryName in @("assets", "i18n", "notices")) {
    $source = Join-Path $BuildDirectory $directoryName
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Required package directory is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination $destination -Recurse
}

foreach ($readmeName in @("README.md", "README.en.md")) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $readmeName) `
        -Destination (Join-Path $destination $readmeName)
}
New-Item -Path (Join-Path $destination "output") `
    -ItemType Directory | Out-Null

$forbidden = Get-ChildItem -LiteralPath $destination -Recurse -File |
    Where-Object {
        $_.Name -match "(?i)(^NRI\.dll$|NRD|\.pdb$|\.lib$|\.exp$|\.log$)"
    }
if ($forbidden) {
    $names = ($forbidden | ForEach-Object FullName) -join ", "
    throw "Forbidden development/retired files entered the package: $names"
}

$destinationPrefix = $destination.TrimEnd("\") + "\"
$hashLines = Get-ChildItem -LiteralPath $destination -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($destinationPrefix.Length)
        $relative = $relative.Replace("\", "/")
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$($hash.ToLowerInvariant()) *$relative"
    }
[System.IO.File]::WriteAllLines(
    (Join-Path $destination "SHA256SUMS.txt"),
    $hashLines,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Output $destination
