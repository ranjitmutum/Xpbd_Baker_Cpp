[CmdletBinding()]
param(
    [switch]$Check,
    [string]$Validator = "",
    [string]$ShaderDirectory = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $ShaderDirectory) {
    $ShaderDirectory = Join-Path $repoRoot "src\gfx\spirv"
}
if (-not $Validator) {
    $Validator = Join-Path $PSScriptRoot "glslang\bin\glslangValidator.exe"
}

$Validator = [System.IO.Path]::GetFullPath($Validator)
$ShaderDirectory = [System.IO.Path]::GetFullPath($ShaderDirectory)
if (-not (Test-Path -LiteralPath $Validator -PathType Leaf)) {
    throw "glslangValidator was not found at '$Validator'. Run tools\setup_tools.ps1 first."
}
if (-not (Test-Path -LiteralPath $ShaderDirectory -PathType Container)) {
    throw "Shader directory was not found at '$ShaderDirectory'."
}

$shaderNames = @(
    "mesh.vert",
    "mesh.frag",
    "mesh_rt.vert",
    "mesh_rt.frag",
    "static_mesh.vert",
    "static_mesh.frag",
    "static_mesh_rt.vert",
    "static_mesh_rt.frag",
    "path_trace.comp",
    "rt_debug.rgen",
    "rt_debug.rmiss",
    "rt_shadow.rmiss",
    "rt_debug.rchit",
    "rt_debug.rahit",
    "pt_composite.vert",
    "pt_composite.frag",
    "skybox.vert",
    "skybox.frag",
    "atmosphere_transmittance.comp",
    "atmosphere_direct_irradiance.comp",
    "atmosphere_single_scattering.comp",
    "atmosphere_scattering_density.comp",
    "atmosphere_indirect_irradiance.comp",
    "atmosphere_multiple_scattering.comp",
    "atmosphere_environment_cache.comp",
    "ui.vert",
    "ui.frag"
)
$tempDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("xpbd-spirv-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDirectory | Out-Null

function Convert-SpirvToInclude {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinaryPath,
        [Parameter(Mandatory = $true)]
        [string]$SourceName
    )

    $bytes = [System.IO.File]::ReadAllBytes($BinaryPath)
    if (($bytes.Length % 4) -ne 0) {
        throw "SPIR-V output '$BinaryPath' is not 32-bit word aligned."
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("// Generated from $SourceName by tools/build_spirv.ps1. Do not edit by hand.")
    $words = [System.Collections.Generic.List[string]]::new()
    for ($offset = 0; $offset -lt $bytes.Length; $offset += 4) {
        $word = [uint32]$bytes[$offset]
        $word = $word -bor ([uint32]$bytes[$offset + 1] -shl 8)
        $word = $word -bor ([uint32]$bytes[$offset + 2] -shl 16)
        $word = $word -bor ([uint32]$bytes[$offset + 3] -shl 24)
        $words.Add(("0x{0:x8}u" -f $word))
    }
    for ($index = 0; $index -lt $words.Count; $index += 6) {
        $last = [Math]::Min($index + 5, $words.Count - 1)
        $slice = $words.GetRange($index, $last - $index + 1)
        $lines.Add("  " + ([string]::Join(", ", $slice)) + ",")
    }
    return ([string]::Join("`n", $lines) + "`n")
}

try {
    $stale = [System.Collections.Generic.List[string]]::new()
    foreach ($shaderName in $shaderNames) {
        $sourcePath = Join-Path $ShaderDirectory $shaderName
        $includePath = "$sourcePath.spv.inc"
        $binaryOutputPath = "$sourcePath.spv"
        $binaryPath = Join-Path $tempDirectory "$shaderName.spv"
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Shader source was not found at '$sourcePath'."
        }

        # Windows PowerShell turns native stderr into a terminating ErrorRecord when the
        # script-wide preference is Stop. glslang writes the source path to stderr even on
        # success, so capture it under Continue and decide from the process exit code.
        # Ray-query / path-trace shaders need SPIR-V 1.4 / Vulkan 1.2 environment.
        $targetEnv = if ($shaderName -match "_rt\.(vert|frag)$" -or
                         $shaderName -match "path_trace\.comp$" -or
                         $shaderName -match "\.(rgen|rmiss|rchit|rahit)$") {
            "vulkan1.2"
        } else {
            "vulkan1.0"
        }
        $savedErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            $validatorOutput = & $Validator -V --target-env $targetEnv "-I$repoRoot" -o $binaryPath $sourcePath 2>&1
            $validatorExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        if ($validatorExitCode -ne 0) {
            throw "glslangValidator failed for '$sourcePath':`n$($validatorOutput -join "`n")"
        }
        $generated = Convert-SpirvToInclude -BinaryPath $binaryPath -SourceName $shaderName

        if ($Check) {
            $generatedBytes = [System.IO.File]::ReadAllBytes($binaryPath)
            $binaryMatches = Test-Path -LiteralPath $binaryOutputPath -PathType Leaf
            if ($binaryMatches) {
                $currentBytes = [System.IO.File]::ReadAllBytes($binaryOutputPath)
                $binaryMatches = $currentBytes.Length -eq $generatedBytes.Length
                for ($i = 0; $binaryMatches -and $i -lt $generatedBytes.Length; $i++) {
                    $binaryMatches = $currentBytes[$i] -eq $generatedBytes[$i]
                }
            }
            if (-not $binaryMatches) {
                $stale.Add($binaryOutputPath)
            }
            $current = if (Test-Path -LiteralPath $includePath -PathType Leaf) {
                [System.IO.File]::ReadAllText($includePath).Replace("`r`n", "`n")
            } else {
                ""
            }
            if ($current -ne $generated) {
                $stale.Add($includePath)
            }
            continue
        }

        [System.IO.File]::WriteAllBytes(
            $binaryOutputPath,
            [System.IO.File]::ReadAllBytes($binaryPath)
        )
        [System.IO.File]::WriteAllText(
            $includePath,
            $generated,
            [System.Text.UTF8Encoding]::new($false)
        )
        Write-Host "Generated $binaryOutputPath"
        Write-Host "Generated $includePath"
    }

    if ($Check -and $stale.Count -gt 0) {
        throw "Embedded SPIR-V is stale:`n  $($stale -join "`n  ")`nRun tools\build_spirv.ps1 and rebuild."
    }
    if ($Check) {
        Write-Host "Embedded SPIR-V is up to date."
    }
} finally {
    if (Test-Path -LiteralPath $tempDirectory) {
        Remove-Item -LiteralPath $tempDirectory -Recurse -Force
    }
}
