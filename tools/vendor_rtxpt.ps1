# Optional: place NVIDIA RTXPT sample sources under third_party for developers.
# https://github.com/NVIDIA-RTX/RTXPT
#
# This is a local git clone helper for engineering reference only (not used by
# the end-user application binary at runtime).
#
# Usage (from repo root, developer machine):
#   powershell -File tools/vendor_rtxpt.ps1
#   powershell -File tools/vendor_rtxpt.ps1 -WithAssets
#   powershell -File tools/vendor_rtxpt.ps1 -Tag v1.8.1

[CmdletBinding()]
param(
    [string]$Destination = "",
    [string]$Tag = "main",
    [switch]$WithAssets,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Destination) {
    $Destination = Join-Path $repoRoot "third_party\RTXPT"
}
$Destination = [System.IO.Path]::GetFullPath($Destination)

Write-Host "RTXPT destination: $Destination"
Write-Host "Tag/branch: $Tag"

if ((Test-Path -LiteralPath $Destination) -and -not $Force) {
    if (Test-Path -LiteralPath (Join-Path $Destination ".git")) {
        Write-Host "RTXPT already present. Use -Force to re-clone."
        Push-Location $Destination
        try {
            git fetch --tags origin 2>&1 | Out-Host
            git checkout $Tag 2>&1 | Out-Host
            git pull --ff-only origin $Tag 2>&1 | Out-Host
            if ($WithAssets) {
                git submodule update --init --recursive External Assets 2>&1 | Out-Host
            } else {
                # Code + engine deps only (skip multi-GB Assets pack by default).
                git submodule update --init --recursive External 2>&1 | Out-Host
            }
        } finally {
            Pop-Location
        }
        Write-Host "Updated existing RTXPT tree."
        exit 0
    }
    throw "Path exists but is not a git repo: $Destination (pass -Force to replace)"
}

if ($Force -and (Test-Path -LiteralPath $Destination)) {
    Remove-Item -LiteralPath $Destination -Recurse -Force
}

$parent = Split-Path -Parent $Destination
if (-not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent | Out-Null
}

# Shallow clone of the sample; then pull submodules (Donut, NRD, RTXDI, …).
git clone --branch $Tag --single-branch --depth 1 `
    https://github.com/NVIDIA-RTX/RTXPT.git $Destination
if ($LASTEXITCODE -ne 0) {
    throw "git clone RTXPT failed with exit $LASTEXITCODE"
}

Push-Location $Destination
try {
    if ($WithAssets) {
        git submodule update --init --recursive 2>&1 | Out-Host
    } else {
        git submodule update --init --recursive External 2>&1 | Out-Host
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Submodule update reported errors (network/proxy). Retry later."
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "Vendor sample tree is ready."
Write-Host "Next (developers):"
Write-Host "  cmake -S . -B build -DXPBD_WITH_RTXPT=ON -DXPBD_RTXPT_ROOT=`"$Destination`""
Write-Host "See docs/rtxpt_integration.md."
