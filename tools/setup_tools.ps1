# Developer helper: obtain Khronos glslangValidator for local shader builds.
# End-user app packages already ship compiled SPIR-V and do not need this.
# Run from repo:  powershell -File tools/setup_tools.ps1

$ErrorActionPreference = "Stop"
$ToolsDir = $PSScriptRoot
$DestDir = Join-Path $ToolsDir "glslang"
$ZipPath = Join-Path $ToolsDir "glslang.zip"

# Official Khronos continuous Windows build (shader compiler only).
$Url = "https://github.com/KhronosGroup/glslang/releases/download/main-tot/glslang-master-windows-Release.zip"

function Test-Glslang {
    $v = Join-Path $DestDir "bin\glslangValidator.exe"
    if (-not (Test-Path $v)) {
        $v = Join-Path $DestDir "glslangValidator.exe"
    }
    return (Test-Path $v)
}

if (Test-Glslang) {
    Write-Host "glslang already present under $DestDir"
    Get-ChildItem -Path $DestDir -Recurse -Filter "glslangValidator.exe" | ForEach-Object {
        Write-Host "  $($_.FullName)"
    }
    exit 0
}

Write-Host "Obtaining glslangValidator (shader compiler) for local development..."
Write-Host "  $Url"
try {
    # .NET WebClient is common for build scripts; package is a known open-source tool.
    $wc = New-Object System.Net.WebClient
    $wc.DownloadFile($Url, $ZipPath)
} catch {
    Write-Error "Could not obtain glslang package: $_"
    exit 1
}

Write-Host "Unpacking to $DestDir ..."
if (Test-Path $DestDir) {
    Remove-Item -Recurse -Force $DestDir
}
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
Expand-Archive -Path $ZipPath -DestinationPath $DestDir -Force

# Normalize layout: some zips put bins at root, some under bin/
$validator = Get-ChildItem -Path $DestDir -Recurse -Filter "glslangValidator.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $validator) {
    Write-Error "glslangValidator.exe not found after extract. Check $DestDir"
    exit 1
}

# Ensure bin/glslangValidator.exe for our scripts
$binDir = Join-Path $DestDir "bin"
if (-not (Test-Path (Join-Path $binDir "glslangValidator.exe"))) {
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    Copy-Item $validator.FullName (Join-Path $binDir "glslangValidator.exe") -Force
    $glslang = Get-ChildItem -Path $DestDir -Recurse -Filter "glslang.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($glslang) {
        Copy-Item $glslang.FullName (Join-Path $binDir "glslang.exe") -Force
    }
}

Write-Host "OK: $($validator.FullName)"
Write-Host "Optional: remove cache zip $ZipPath to save space."
exit 0
