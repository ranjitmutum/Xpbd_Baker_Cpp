# Download and unpack tools that are too large for git (glslangValidator).
# Run from repo:  powershell -File cpp/tools/setup_tools.ps1
# Or:             cpp\tools\setup_tools.bat

$ErrorActionPreference = "Stop"
$ToolsDir = $PSScriptRoot
$DestDir = Join-Path $ToolsDir "glslang"
$ZipPath = Join-Path $ToolsDir "glslang.zip"

# Continuous Windows Release package from Khronos (prebuilt validator).
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

Write-Host "Downloading glslang (Windows Release)..."
Write-Host "  $Url"
try {
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
} catch {
    Write-Error "Download failed: $_"
    exit 1
}

Write-Host "Extracting to $DestDir ..."
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
