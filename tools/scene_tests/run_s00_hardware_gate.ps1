<#
.SYNOPSIS
Runs repeatable S00-A hot-import, S00-C visibility, or S00-D Still cases.

.DESCRIPTION
Each child application is watched through the exact Process object returned by
System.Diagnostics.Process.Start. Every run writes command, environment, process,
raw-log, metrics,
and SHA-256 evidence below EvidenceRoot/<case>/<UTC timestamp>/run_NNN.

CaptureWindow is optional and disabled by default. When enabled, the child GUI
is shown and lossless PNG frames are captured from the Win32/DWM bounds of that
exact PID's main window; capture attempts and rectangles are included in metrics.

The runner never force-terminates a normally running application. For a Still
case it requests a normal main-window close after observing the completed Still
lifecycle. A force termination is allowed only after the wall-clock timeout has
been recorded in process.json and runner.log.

Additional child environment values use a NAME=VALUE array, for example:

  -Env 'XPBD_PREVIEW_SCENE=8','XPBD_PT_FRAME_GENERATION=0'

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tools\scene_tests\run_s00_hardware_gate.ps1 `
  -Case HotImport `
  -StartupModel artifacts\scene_stage_S00\generated_fixtures\bedrock_s00_small_8cubes.geo.json `
  -HotImportModel artifacts\scene_stage_S00\generated_fixtures\bedrock_s00_stress_10368cubes_144bones.geo.json `
  -DialogHoldMilliseconds 2000 `
  -Env 'XPBD_PT_FRAME_GENERATION=0'

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tools\scene_tests\run_s00_hardware_gate.ps1 `
  -Case Still -RepeatCount 1 `
  -StartupModel artifacts\scene_stage_S00\generated_fixtures\bedrock_s00_small_8cubes.geo.json `
  -Env 'XPBD_PREVIEW_SCENE=8','XPBD_STILL_WIDTH=64',`
       'XPBD_STILL_HEIGHT=64','XPBD_STILL_SAMPLES=4'
#>

[CmdletBinding()]
param(
    [string]$App = ".\out\build\s00-gate\Release\xpbd_baker_app.exe",
    [string]$Spec = ".\assets\scene_tests\s00\s00_gate_spec.json",
    [string]$EvidenceRoot = ".\artifacts\scene_stage_S00",
    [ValidateSet("Validate", "HotImport", "Visibility", "Still")]
    [string]$Case = "Validate",
    [string]$CaseName = "",
    [string]$StartupModel = "",
    [string]$HotImportModel = "",
    [string]$VisibilityTarget = "",
    [ValidateRange(0, 60000)]
    [int]$DialogHoldMilliseconds = 0,
    [ValidateRange(0, 1000)]
    [int]$RepeatCount = 0,
    [Alias("Environment")]
    [string[]]$Env = @(),
    [string]$EnvironmentFile = "",
    [string[]]$ArgumentList = @(),
    [ValidateRange(0, 86400)]
    [int]$WallTimeoutSeconds = 0,
    [ValidateRange(25, 5000)]
    [int]$PollMilliseconds = 100,
    [ValidateRange(1, 300)]
    [int]$GracefulCloseSeconds = 15,
    [ValidateRange(0, 86400)]
    [int]$ImportTimeoutSeconds = 0,
    [switch]$CaptureWindow,
    [ValidateRange(100, 60000)]
    [int]$WindowCaptureIntervalMilliseconds = 500,
    [ValidateRange(1, 1000)]
    [int]$MaximumWindowCaptureFrames = 16,
    [switch]$LeaveStillWindowOpen,
    [switch]$Help
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:RunnerPath = $MyInvocation.MyCommand.Path
$script:RepositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot "..\..")
)

function Resolve-RepositoryPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [switch]$RequireFile
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        $resolved = [System.IO.Path]::GetFullPath($Path)
    } else {
        $resolved = [System.IO.Path]::GetFullPath(
            (Join-Path $script:RepositoryRoot $Path)
        )
    }
    if ($RequireFile -and
        -not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "Required file is missing: $resolved"
    }
    return $resolved
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $json = $Value | ConvertTo-Json -Depth 24
    [System.IO.File]::WriteAllText($Path, $json + "`n", $script:Utf8NoBom)
}

function Add-RunnerLog {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $line = "[{0}] {1}`r`n" -f `
        [DateTime]::UtcNow.ToString("o"), $Message
    [System.IO.File]::AppendAllText($Path, $line, $script:Utf8NoBom)
    [Console]::WriteLine($line.TrimEnd())
}

function Read-SharedText {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor `
            [System.IO.FileShare]::Delete
        $stream = New-Object System.IO.FileStream(
            $Path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            $share
        )
        try {
            $reader = New-Object System.IO.StreamReader(
                $stream,
                [System.Text.Encoding]::UTF8,
                $true
            )
            try {
                return $reader.ReadToEnd()
            } finally {
                $reader.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
    } catch {
        return ""
    }
}

function Get-Sha256Record {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{
            path = $Path
            exists = $false
            bytes = $null
            sha256 = $null
        }
    }
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName
        exists = $true
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-GitMetadata {
    $metadata = [ordered]@{
        commit = $null
        dirty = $null
        status_porcelain = @()
        error = $null
    }
    try {
        $git = Get-Command git -ErrorAction Stop
        $commit = & $git.Source -C $script:RepositoryRoot rev-parse HEAD `
            2>$null
        if ($LASTEXITCODE -ne 0) {
            throw "git rev-parse returned $LASTEXITCODE"
        }
        $status = @(& $git.Source -C $script:RepositoryRoot `
            status --porcelain=v1 --untracked-files=normal 2>$null)
        if ($LASTEXITCODE -ne 0) {
            throw "git status returned $LASTEXITCODE"
        }
        $metadata.commit = ([string]$commit).Trim()
        $metadata.status_porcelain = $status
        $metadata.dirty = $status.Count -gt 0
    } catch {
        $metadata.error = $_.Exception.Message
    }
    return $metadata
}

function Get-HardwareMetadata {
    $result = [ordered]@{
        computer_name = $env:COMPUTERNAME
        os = $null
        gpu = @()
        culture = [System.Globalization.CultureInfo]::CurrentCulture.Name
        ui_culture = [System.Globalization.CultureInfo]::CurrentUICulture.Name
        powershell = $PSVersionTable.PSVersion.ToString()
        collection_errors = @()
    }
    try {
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        $result.os = [ordered]@{
            caption = $os.Caption
            version = $os.Version
            build_number = $os.BuildNumber
            architecture = $os.OSArchitecture
        }
    } catch {
        $result.collection_errors += "OS: $($_.Exception.Message)"
    }
    try {
        $controllers = @(Get-CimInstance Win32_VideoController `
            -ErrorAction Stop)
        $result.gpu = @($controllers | ForEach-Object {
            [ordered]@{
                name = $_.Name
                driver_version = $_.DriverVersion
                driver_date = if ($_.DriverDate) {
                    ([DateTime]$_.DriverDate).ToUniversalTime().ToString("o")
                } else {
                    $null
                }
                adapter_ram = $_.AdapterRAM
                pnp_device_id = $_.PNPDeviceID
            }
        })
    } catch {
        $result.collection_errors += "GPU: $($_.Exception.Message)"
    }
    return $result
}

function Get-RuntimeDllMetadata {
    param([Parameter(Mandatory = $true)][string]$AppDirectory)

    $names = @(
        "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll",
        "sl.dlss_d.dll", "sl.dlss_g.dll", "sl.pcl.dll",
        "sl.reflex.dll", "nvngx_dlss.dll", "nvngx_dlssd.dll",
        "nvngx_dlssg.dll", "vulkan-1.dll"
    )
    return @($names | ForEach-Object {
        $path = Join-Path $AppDirectory $_
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
            [ordered]@{
                name = $_
                file_version = $version.FileVersion
                product_version = $version.ProductVersion
                bytes = (Get-Item -LiteralPath $path).Length
                sha256 = (Get-FileHash -LiteralPath $path `
                    -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    })
}

function ConvertTo-WindowsCommandLineArgument {
    param([AllowEmptyString()][string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            ++$backslashes
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Initialize-WindowCapture {
    Add-Type -AssemblyName System.Drawing
    $nativeType = [System.Management.Automation.PSTypeName]
        'XpbdS00WindowCaptureNative'
    if ($null -ne $nativeType.Type) {
        return
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class XpbdS00WindowCaptureNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(
        IntPtr hwnd,
        out uint processId
    );

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(
        IntPtr hwnd,
        uint attribute,
        out RECT value,
        int valueSize
    );
}
'@
}

function Save-ExactProcessWindowFrame {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string]$Destination,
        [Parameter(Mandatory = $true)]
        [int]$Attempt
    )

    $record = [ordered]@{
        attempt = $Attempt
        utc = [DateTime]::UtcNow.ToString('o')
        exact_pid = $Process.Id
        window_handle = $null
        window_title = $null
        rect = $null
        path = $null
        success = $false
        error = $null
    }
    $bitmap = $null
    $graphics = $null
    try {
        $Process.Refresh()
        $handle = $Process.MainWindowHandle
        $record.window_handle = ('0x{0:x}' -f $handle.ToInt64())
        $record.window_title = $Process.MainWindowTitle
        if ($handle -eq [IntPtr]::Zero) {
            throw 'exact PID has no main window yet'
        }
        [uint32]$ownerPid = 0
        [void][XpbdS00WindowCaptureNative]::GetWindowThreadProcessId(
            $handle, [ref]$ownerPid
        )
        if ($ownerPid -ne [uint32]$Process.Id) {
            throw "window PID $ownerPid does not match exact PID $($Process.Id)"
        }
        if (-not [XpbdS00WindowCaptureNative]::IsWindowVisible($handle)) {
            throw 'exact PID main window is not visible'
        }
        if ([XpbdS00WindowCaptureNative]::IsIconic($handle)) {
            throw 'exact PID main window is minimized'
        }

        $rect = New-Object XpbdS00WindowCaptureNative+RECT
        $dwmResult = [XpbdS00WindowCaptureNative]::DwmGetWindowAttribute(
            $handle, 9, [ref]$rect,
            [Runtime.InteropServices.Marshal]::SizeOf($rect)
        )
        if ($dwmResult -ne 0) {
            if (-not [XpbdS00WindowCaptureNative]::GetWindowRect(
                    $handle, [ref]$rect
                )) {
                throw "GetWindowRect failed after DWM result $dwmResult"
            }
        }
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
        if ($width -le 0 -or $height -le 0) {
            throw "invalid window bounds ${width}x${height}"
        }
        $record.rect = [ordered]@{
            left = $rect.Left
            top = $rect.Top
            right = $rect.Right
            bottom = $rect.Bottom
            width = $width
            height = $height
            source = if ($dwmResult -eq 0) {
                'DWMWA_EXTENDED_FRAME_BOUNDS'
            } else { 'GetWindowRect' }
        }

        $bitmap = New-Object System.Drawing.Bitmap(
            $width,
            $height,
            [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
        )
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen(
            $rect.Left,
            $rect.Top,
            0,
            0,
            (New-Object System.Drawing.Size($width, $height)),
            [System.Drawing.CopyPixelOperation]::SourceCopy
        )
        $bitmap.Save(
            $Destination,
            [System.Drawing.Imaging.ImageFormat]::Png
        )
        $record.path = $Destination
        $record.success = $true
    } catch {
        $record.error = $_.Exception.Message
    } finally {
        if ($null -ne $graphics) {
            $graphics.Dispose()
        }
        if ($null -ne $bitmap) {
            $bitmap.Dispose()
        }
    }
    return [pscustomobject]$record
}

function Get-ChildEnvironment {
    param(
        [string[]]$Entries,
        [string]$JsonFile
    )

    $values = [ordered]@{}
    if (-not [string]::IsNullOrWhiteSpace($JsonFile)) {
        $resolvedFile = Resolve-RepositoryPath -Path $JsonFile -RequireFile
        $jsonObject = Get-Content -LiteralPath $resolvedFile -Raw `
            -Encoding UTF8 | ConvertFrom-Json
        foreach ($property in $jsonObject.PSObject.Properties) {
            $name = [string]$property.Name
            if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
                throw "Invalid environment name in ${resolvedFile}: $name"
            }
            $values[$name] = [string]$property.Value
        }
    }
    foreach ($entry in $Entries) {
        $separator = $entry.IndexOf('=')
        if ($separator -le 0) {
            throw "Environment entry must be NAME=VALUE: $entry"
        }
        $name = $entry.Substring(0, $separator)
        $value = $entry.Substring($separator + 1)
        if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
            throw "Invalid environment name: $name"
        }
        $values[$name] = $value
    }
    if ($values.Contains("XPBD_LOG_PATH")) {
        throw "XPBD_LOG_PATH is runner-controlled and cannot be overridden"
    }
    return $values
}

function Start-IsolatedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Environment,
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [bool]$ShowWindow = $false
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = -not $ShowWindow
    $startInfo.WindowStyle = if ($ShowWindow) {
        [System.Diagnostics.ProcessWindowStyle]::Normal
    } else {
        [System.Diagnostics.ProcessWindowStyle]::Hidden
    }
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($Arguments.Count -gt 0) {
        $startInfo.Arguments = (@($Arguments | ForEach-Object {
            ConvertTo-WindowsCommandLineArgument -Value $_
        }) -join ' ')
    }

    foreach ($name in @($startInfo.EnvironmentVariables.Keys)) {
        if ([string]$name -like 'XPBD_*') {
            $startInfo.EnvironmentVariables.Remove([string]$name)
        }
    }
    foreach ($name in $Environment.Keys) {
        $startInfo.EnvironmentVariables[[string]$name] = `
            [string]$Environment[$name]
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Process.Start returned false for $FilePath"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process | Add-Member -NotePropertyName S00StdoutTask `
        -NotePropertyValue $stdoutTask
    $process | Add-Member -NotePropertyName S00StderrTask `
        -NotePropertyValue $stderrTask
    $process | Add-Member -NotePropertyName S00StdoutPath `
        -NotePropertyValue $StdoutPath
    $process | Add-Member -NotePropertyName S00StderrPath `
        -NotePropertyValue $StderrPath
    return $process
}

function Find-ForbiddenLogMatches {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [object[]]$Patterns
    )

    $matches = @()
    $lines = $Text -split "`r?`n"
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; ++$lineIndex) {
        foreach ($patternValue in $Patterns) {
            $pattern = [string]$patternValue
            if ($lines[$lineIndex].IndexOf(
                    $pattern,
                    [System.StringComparison]::OrdinalIgnoreCase
                ) -ge 0) {
                $matches += [ordered]@{
                    pattern = $pattern
                    line = $lineIndex + 1
                    text = $lines[$lineIndex]
                }
                if ($matches.Count -ge 100) {
                    return $matches
                }
            }
        }
    }
    return $matches
}

function Get-HeartbeatMetrics {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$LogText
    )

    $timestamps = New-Object System.Collections.Generic.List[DateTime]
    $regex = New-Object System.Text.RegularExpressions.Regex(
        '(?m)^\[(?<time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\].*S00_PRESENT_HEARTBEAT'
    )
    foreach ($match in $regex.Matches($LogText)) {
        $parsed = [DateTime]::MinValue
        if ([DateTime]::TryParseExact(
                $match.Groups['time'].Value,
                'yyyy-MM-dd HH:mm:ss.fff',
                [System.Globalization.CultureInfo]::InvariantCulture,
                [System.Globalization.DateTimeStyles]::None,
                [ref]$parsed
            )) {
            $timestamps.Add($parsed)
        }
    }
    $maximumGap = 0.0
    for ($index = 1; $index -lt $timestamps.Count; ++$index) {
        $gap = ($timestamps[$index] - $timestamps[$index - 1]).TotalSeconds
        if ($gap -gt $maximumGap) {
            $maximumGap = $gap
        }
    }
    return [ordered]@{
        count = $timestamps.Count
        maximum_gap_seconds = $maximumGap
        first = if ($timestamps.Count -gt 0) {
            $timestamps[0].ToString('o')
        } else { $null }
        last = if ($timestamps.Count -gt 0) {
            $timestamps[$timestamps.Count - 1].ToString('o')
        } else { $null }
    }
}

function Get-LastSentinelRecord {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$LogText,
        [Parameter(Mandatory = $true)]
        [string]$Sentinel,
        [string]$RequiredStage = ''
    )

    $lineMatches = [regex]::Matches(
        $LogText,
        '(?m)^.*' + [regex]::Escape($Sentinel) + '[^\r\n]*\r?$'
    )
    for ($index = $lineMatches.Count - 1; $index -ge 0; --$index) {
        $line = $lineMatches[$index].Value
        $fields = [ordered]@{}
        foreach ($fieldMatch in [regex]::Matches(
                $line,
                '(?<key>[A-Za-z_][A-Za-z0-9_]*)=(?<value>[^\s]+)'
            )) {
            $value = $fieldMatch.Groups['value'].Value
            if ($value -match '^\d+$') {
                $fields[$fieldMatch.Groups['key'].Value] = [long]$value
            } else {
                $fields[$fieldMatch.Groups['key'].Value] = $value
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($RequiredStage) -and
            (-not $fields.Contains('stage') -or
             [string]$fields['stage'] -ne $RequiredStage)) {
            continue
        }
        return [pscustomobject]@{
            found = $true
            line = $line
            fields = $fields
        }
    }
    return [pscustomobject]@{
        found = $false
        line = $null
        fields = [ordered]@{}
    }
}

function Get-SentinelInteger {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not $Record.found -or -not $Record.fields.Contains($Name)) {
        return $null
    }
    $number = 0L
    if ([long]::TryParse([string]$Record.fields[$Name], [ref]$number)) {
        return $number
    }
    return $null
}

function Test-ImageSignature {
    param([Parameter(Mandatory = $true)][string]$Path)

    $result = [ordered]@{
        extension = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
        signature_valid = $false
        signature_hex = ""
        validation = "file missing"
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $result
    }
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $buffer = New-Object byte[] 8
        $count = $stream.Read($buffer, 0, $buffer.Length)
        $actual = @()
        for ($index = 0; $index -lt $count; ++$index) {
            $actual += $buffer[$index]
        }
        $result.signature_hex = (($actual | ForEach-Object {
            $_.ToString('x2')
        }) -join '')
        if ($result.extension -eq '.png') {
            $expected = '89504e470d0a1a0a'
            $result.signature_valid = $count -ge 8 -and `
                $result.signature_hex.StartsWith($expected)
            $result.validation = "PNG signature"
        } elseif ($result.extension -eq '.exr') {
            $expected = '762f3101'
            $result.signature_valid = $count -ge 4 -and `
                $result.signature_hex.StartsWith($expected)
            $result.validation = "OpenEXR magic"
        } else {
            $result.validation = "unsupported extension"
        }
    } finally {
        $stream.Dispose()
    }
    return $result
}

function Get-ArtifactHashes {
    param([Parameter(Mandatory = $true)][string]$RunDirectory)

    $prefix = $RunDirectory.TrimEnd('\') + '\'
    return @(
        Get-ChildItem -LiteralPath $RunDirectory -Recurse -File |
            Where-Object { $_.Name -ne 'artifact_sha256.json' } |
            Sort-Object FullName |
            ForEach-Object {
                [ordered]@{
                    path = $_.FullName.Substring($prefix.Length).Replace('\', '/')
                    bytes = $_.Length
                    sha256 = (Get-FileHash -LiteralPath $_.FullName `
                        -Algorithm SHA256).Hash.ToLowerInvariant()
                }
            }
    )
}

function Get-ImportDurationLimit {
    param(
        [int]$CubeCount,
        [object]$GateSpec,
        [int]$Override
    )

    if ($Override -gt 0) {
        return $Override
    }
    if ($CubeCount -le 8) {
        return [int]$GateSpec.timeouts.primary_import_seconds
    }
    if ($CubeCount -le 2624) {
        return [int]$GateSpec.timeouts.extended_import_seconds
    }
    return [int]$GateSpec.timeouts.user_scale_cube_density_import_seconds
}

function Invoke-SingleRun {
    param(
        [Parameter(Mandatory = $true)][int]$Index,
        [Parameter(Mandatory = $true)][int]$Total,
        [Parameter(Mandatory = $true)][string]$RunDirectory,
        [Parameter(Mandatory = $true)][string]$ApplicationPath,
        [Parameter(Mandatory = $true)][string]$ApplicationDirectory,
        [Parameter(Mandatory = $true)][string]$SpecPath,
        [Parameter(Mandatory = $true)][object]$GateSpec,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$BaseEnvironment,
        [Parameter(Mandatory = $true)][object]$GitMetadata,
        [Parameter(Mandatory = $true)][object]$HardwareMetadata,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$RuntimeDllMetadata,
        [Parameter(Mandatory = $true)][string]$ResolvedStartupModel,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$ResolvedHotImportModel,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$ResolvedVisibilityTarget,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    New-Item -ItemType Directory -Path $RunDirectory | Out-Null
    $runnerLog = Join-Path $RunDirectory 'runner.log'
    $appLog = Join-Path $RunDirectory 'xpbd_baker.log'
    $stdoutLog = Join-Path $RunDirectory 'stdout.log'
    $stderrLog = Join-Path $RunDirectory 'stderr.log'
    $processJson = Join-Path $RunDirectory 'process.json'
    $startedUtc = [DateTime]::UtcNow
    $childEnvironment = [ordered]@{}
    foreach ($name in $BaseEnvironment.Keys) {
        $childEnvironment[$name] = [string]$BaseEnvironment[$name]
    }
    $childEnvironment['XPBD_LOG_PATH'] = $appLog

    if ($Case -eq 'HotImport') {
        [void]$childEnvironment.Remove('XPBD_S00_VISIBILITY_TARGET')
        $childEnvironment['XPBD_STILL_RENDER'] = '0'
        $childEnvironment['XPBD_S00_EXIT_AFTER_STILL'] = '0'
        $childEnvironment['XPBD_S00_HOT_IMPORT_MODEL'] = `
            $ResolvedHotImportModel
        $childEnvironment['XPBD_S00_DIALOG_HOLD_MS'] = `
            [string]$DialogHoldMilliseconds
    } elseif ($Case -eq 'Still') {
        [void]$childEnvironment.Remove('XPBD_S00_HOT_IMPORT_MODEL')
        [void]$childEnvironment.Remove('XPBD_S00_VISIBILITY_TARGET')
        $childEnvironment['XPBD_STILL_RENDER'] = '1'
        $childEnvironment['XPBD_S00_EXIT_AFTER_STILL'] = '1'
        if (-not $childEnvironment.Contains('XPBD_STILL_WIDTH')) {
            $childEnvironment['XPBD_STILL_WIDTH'] = `
                [string]$GateSpec.s00_d_still_snapshot.reliability_resolution[0]
        }
        if (-not $childEnvironment.Contains('XPBD_STILL_HEIGHT')) {
            $childEnvironment['XPBD_STILL_HEIGHT'] = `
                [string]$GateSpec.s00_d_still_snapshot.reliability_resolution[1]
        }
        if (-not $childEnvironment.Contains('XPBD_STILL_SAMPLES')) {
            $childEnvironment['XPBD_STILL_SAMPLES'] = `
                [string]$GateSpec.s00_d_still_snapshot.reliability_samples
        }
        if (-not $childEnvironment.Contains('XPBD_STILL_SPP')) {
            $childEnvironment['XPBD_STILL_SPP'] = '2'
        }
        if (-not $childEnvironment.Contains('XPBD_STILL_FORMAT')) {
            $childEnvironment['XPBD_STILL_FORMAT'] = 'png'
        }
        if (-not $childEnvironment.Contains('XPBD_STILL_TRANSPARENT')) {
            $childEnvironment['XPBD_STILL_TRANSPARENT'] = '0'
        }
        $requestedName = if ($childEnvironment.Contains('XPBD_STILL_FILENAME')) {
            [string]$childEnvironment['XPBD_STILL_FILENAME']
        } else {
            's00-still'
        }
        $safeStem = [System.IO.Path]::GetFileNameWithoutExtension($requestedName)
        $safeStem = $safeStem -replace '[^A-Za-z0-9._-]', '_'
        if ([string]::IsNullOrWhiteSpace($safeStem)) {
            $safeStem = 's00-still'
        }
        $childEnvironment['XPBD_STILL_FILENAME'] = `
            '{0}-r{1:D3}-{2}' -f $safeStem, $Index, `
            $startedUtc.ToString('yyyyMMddTHHmmssfffZ')
    } else {
        [void]$childEnvironment.Remove('XPBD_S00_HOT_IMPORT_MODEL')
        $childEnvironment['XPBD_S00_VISIBILITY_TARGET'] = `
            $ResolvedVisibilityTarget
        if (-not $childEnvironment.Contains('XPBD_PT_DENOISER')) {
            $childEnvironment['XPBD_PT_DENOISER'] = 'raw'
        }
        if (-not $childEnvironment.Contains('XPBD_PT_UPSCALE')) {
            $childEnvironment['XPBD_PT_UPSCALE'] = 'off'
        }
        if (-not $childEnvironment.Contains('XPBD_PT_FRAME_GENERATION')) {
            $childEnvironment['XPBD_PT_FRAME_GENERATION'] = '0'
        }
        $childEnvironment['XPBD_STILL_RENDER'] = '0'
        $childEnvironment['XPBD_S00_EXIT_AFTER_STILL'] = '0'
    }
    if (-not [string]::IsNullOrWhiteSpace($ResolvedStartupModel)) {
        $childEnvironment['XPBD_MODEL'] = $ResolvedStartupModel
    }

    $inputBefore = [ordered]@{}
    if (-not [string]::IsNullOrWhiteSpace($ResolvedStartupModel)) {
        $inputBefore.startup_model = `
            Get-Sha256Record -Path $ResolvedStartupModel
    }
    if (-not [string]::IsNullOrWhiteSpace($ResolvedHotImportModel)) {
        $inputBefore.hot_import_model = `
            Get-Sha256Record -Path $ResolvedHotImportModel
    }
    if ($Case -eq 'Visibility' -and
        $childEnvironment.Contains('XPBD_TEXTURE') -and
        $childEnvironment.Contains('XPBD_LABPBR_SPECULAR')) {
        $inputBefore.visibility_base_texture = Get-Sha256Record `
            -Path ([string]$childEnvironment['XPBD_TEXTURE'])
        $inputBefore.visibility_labpbr_specular = Get-Sha256Record `
            -Path ([string]$childEnvironment['XPBD_LABPBR_SPECULAR'])
    }

    $commandRecord = [ordered]@{
        schema_version = 1
        gate_version = $GateSpec.gate_version
        case = $Case
        repeat_index = $Index
        repeat_total = $Total
        runner = $script:RunnerPath
        app = Get-Sha256Record -Path $ApplicationPath
        spec = Get-Sha256Record -Path $SpecPath
        working_directory = $ApplicationDirectory
        argument_list = @($ArgumentList)
        display_command = $ApplicationPath + $(if ($ArgumentList.Count -gt 0) {
            ' ' + (@($ArgumentList | ForEach-Object {
                ConvertTo-WindowsCommandLineArgument -Value $_
            }) -join ' ')
        } else { '' })
        started_utc = $startedUtc.ToString('o')
        result_commit = $GitMetadata.commit
        dirty = $GitMetadata.dirty
        input_before = $inputBefore
        window_capture = [ordered]@{
            enabled = [bool]$CaptureWindow
            exact_pid_window_rect = [bool]$CaptureWindow
            interval_milliseconds = $WindowCaptureIntervalMilliseconds
            maximum_frames = $MaximumWindowCaptureFrames
        }
    }
    Write-JsonFile -Path (Join-Path $RunDirectory 'command.json') `
        -Value $commandRecord

    $environmentRecord = [ordered]@{
        schema_version = 1
        isolated_xpbd_environment = $true
        child_environment = $childEnvironment
        hardware = $HardwareMetadata
        runtime_dlls = $RuntimeDllMetadata
        git = $GitMetadata
    }
    Write-JsonFile -Path (Join-Path $RunDirectory 'environment.json') `
        -Value $environmentRecord

    Add-RunnerLog -Path $runnerLog -Message (
        "RUN_BEGIN case={0} repeat={1}/{2} wall_timeout_seconds={3}" -f `
        $Case, $Index, $Total, $TimeoutSeconds
    )

    $process = $null
    $timedOut = $false
    $timeoutRecordedBeforeForce = $false
    $forcedTermination = $false
    $closeAttempted = $false
    $closeRequested = $false
    $stillCompleteObserved = $false
    $stillCompleteObservedAt = $null
    $launchError = $null
    $monitorErrors = @()
    $windowCaptureRecords = @()
    $windowCaptureAttemptCount = 0
    $windowCaptureSuccessCount = 0
    $windowCaptureInitializationError = $null
    $nextWindowCaptureUtc = $startedUtc
    $visibilityCapturePhases = [ordered]@{
        baseline = $false
        hidden = $false
        restored = $false
    }

    if ($CaptureWindow) {
        try {
            Initialize-WindowCapture
        } catch {
            $windowCaptureInitializationError = $_.Exception.Message
            Add-RunnerLog -Path $runnerLog -Message (
                "WINDOW_CAPTURE_INITIALIZATION_FAILED error={0}" -f `
                $windowCaptureInitializationError
            )
        }
    }

    try {
        $process = Start-IsolatedProcess -FilePath $ApplicationPath `
            -WorkingDirectory $ApplicationDirectory `
            -Environment $childEnvironment -Arguments $ArgumentList `
            -StdoutPath $stdoutLog -StderrPath $stderrLog `
            -ShowWindow ([bool]$CaptureWindow)
        Add-RunnerLog -Path $runnerLog -Message (
            "PROCESS_STARTED exact_pid={0}" -f $process.Id
        )
    } catch {
        $launchError = $_.Exception.Message
        Add-RunnerLog -Path $runnerLog -Message (
            "PROCESS_LAUNCH_FAILED error={0}" -f $launchError
        )
    }

    if ($null -ne $process) {
        Write-JsonFile -Path $processJson -Value ([ordered]@{
            schema_version = 1
            exact_pid_watchdog = $true
            pid = $process.Id
            state = 'Running'
            started_utc = $startedUtc.ToString('o')
            wall_timeout_seconds = $TimeoutSeconds
            timed_out = $false
            forced_termination = $false
        })

        $deadline = $startedUtc.AddSeconds($TimeoutSeconds)
        while (-not $process.HasExited) {
            try {
                if ($CaptureWindow -and $Case -ne 'Visibility' -and
                    $null -eq $windowCaptureInitializationError -and
                    $windowCaptureSuccessCount -lt
                        $MaximumWindowCaptureFrames -and
                    $windowCaptureAttemptCount -lt
                        ($MaximumWindowCaptureFrames * 20) -and
                    [DateTime]::UtcNow -ge $nextWindowCaptureUtc) {
                    ++$windowCaptureAttemptCount
                    $capturePath = Join-Path $RunDirectory (
                        'window_frame_attempt_{0:D4}.png' -f `
                        $windowCaptureAttemptCount
                    )
                    $captureRecord = Save-ExactProcessWindowFrame `
                        -Process $process -Destination $capturePath `
                        -Attempt $windowCaptureAttemptCount
                    $captureRecord | Add-Member -NotePropertyName phase `
                        -NotePropertyValue 'interval'
                    $windowCaptureRecords += $captureRecord
                    if ($captureRecord.success) {
                        ++$windowCaptureSuccessCount
                    }
                    $nextWindowCaptureUtc = [DateTime]::UtcNow.AddMilliseconds(
                        $WindowCaptureIntervalMilliseconds
                    )
                }
                if ($CaptureWindow -and $Case -eq 'Visibility' -and
                    $null -eq $windowCaptureInitializationError -and
                    $windowCaptureSuccessCount -lt
                        $MaximumWindowCaptureFrames -and
                    $windowCaptureAttemptCount -lt
                        ($MaximumWindowCaptureFrames * 20) -and
                    [DateTime]::UtcNow -ge $nextWindowCaptureUtc) {
                    $visibilityLog = Read-SharedText -Path $appLog
                    $latestVisibilityHeartbeat = Get-LastSentinelRecord `
                        -LogText $visibilityLog `
                        -Sentinel 'S00_VISIBILITY_HEARTBEAT'
                    $capturePhase = $null
                    if ($latestVisibilityHeartbeat.found -and
                        $latestVisibilityHeartbeat.fields.Contains('stage')) {
                        $latestStage = [string]
                            $latestVisibilityHeartbeat.fields['stage']
                        if ($latestStage -eq 'await_rt_ready' -and
                            -not $visibilityCapturePhases.baseline) {
                            $readyCount = Get-SentinelInteger `
                                -Record $latestVisibilityHeartbeat `
                                -Name 'ready_count'
                            if ($null -ne $readyCount -and $readyCount -ge 5) {
                                $capturePhase = 'baseline'
                            }
                        } elseif ($latestStage -eq 'hidden' -and
                            -not $visibilityCapturePhases.hidden -and
                            $visibilityLog -match `
                                'S00_VISIBILITY_HIDDEN_OBSERVED') {
                            $capturePhase = 'hidden'
                        } elseif ($latestStage -eq 'restored' -and
                            -not $visibilityCapturePhases.restored -and
                            $visibilityLog -match `
                                'S00_VISIBILITY_RESTORED_OBSERVED') {
                            $capturePhase = 'restored'
                        }
                    }
                    if ($null -ne $capturePhase) {
                        ++$windowCaptureAttemptCount
                        $capturePath = Join-Path $RunDirectory (
                            'window_visibility_{0}_attempt_{1:D4}.png' -f `
                            $capturePhase, $windowCaptureAttemptCount
                        )
                        $captureRecord = Save-ExactProcessWindowFrame `
                            -Process $process -Destination $capturePath `
                            -Attempt $windowCaptureAttemptCount
                        $captureRecord | Add-Member -NotePropertyName phase `
                            -NotePropertyValue $capturePhase
                        $windowCaptureRecords += $captureRecord
                        if ($captureRecord.success) {
                            ++$windowCaptureSuccessCount
                            $visibilityCapturePhases[$capturePhase] = $true
                        }
                        $nextWindowCaptureUtc = `
                            [DateTime]::UtcNow.AddMilliseconds(
                                $WindowCaptureIntervalMilliseconds
                            )
                    }
                }
                if ($Case -eq 'Still') {
                    $currentLog = Read-SharedText -Path $appLog
                    if (-not $stillCompleteObserved -and $currentLog -match `
                        '(?m)STILL_JOB complete job_id=\d+') {
                        $stillCompleteObserved = $true
                        $stillCompleteObservedAt = [DateTime]::UtcNow
                        Add-RunnerLog -Path $runnerLog -Message (
                            (
                                "STILL_COMPLETE_OBSERVED exact_pid={0} " +
                                "awaiting_s00_clean_exit=true"
                            ) -f $process.Id
                        )
                    }
                    if ($stillCompleteObserved -and -not $closeAttempted -and
                        -not $LeaveStillWindowOpen -and
                        [DateTime]::UtcNow -ge
                            $stillCompleteObservedAt.AddSeconds(
                                $GracefulCloseSeconds
                            )) {
                        $closeAttempted = $true
                        $closeRequested = $process.CloseMainWindow()
                        Add-RunnerLog -Path $runnerLog -Message (
                            (
                                "STILL_CLEAN_EXIT_GRACE_EXPIRED exact_pid={0} " +
                                "close_request_accepted={1} grace_seconds={2}"
                            ) -f $process.Id, $closeRequested, `
                                $GracefulCloseSeconds
                        )
                    }
                }
            } catch {
                $monitorErrors += $_.Exception.Message
            }

            if ([DateTime]::UtcNow -ge $deadline) {
                $timedOut = $true
                Add-RunnerLog -Path $runnerLog -Message (
                    "WALL_TIMEOUT_RECORDED exact_pid={0} force_termination_pending=true" -f `
                    $process.Id
                )
                Write-JsonFile -Path $processJson -Value ([ordered]@{
                    schema_version = 1
                    exact_pid_watchdog = $true
                    pid = $process.Id
                    state = 'WallTimeoutRecordedBeforeForceTermination'
                    started_utc = $startedUtc.ToString('o')
                    timeout_recorded_utc = [DateTime]::UtcNow.ToString('o')
                    wall_timeout_seconds = $TimeoutSeconds
                    timed_out = $true
                    force_termination_pending = $true
                    forced_termination = $false
                })
                $timeoutRecordedBeforeForce = $true
                try {
                    if (-not $process.HasExited) {
                        $process.Kill()
                        $forcedTermination = $true
                    }
                } catch {
                    $monitorErrors += "Timeout termination: $($_.Exception.Message)"
                }
                break
            }
            Start-Sleep -Milliseconds $PollMilliseconds
            try {
                $process.Refresh()
            } catch {
                $monitorErrors += "Process refresh: $($_.Exception.Message)"
            }
        }
        $processExitedForDrain = $false
        try {
            $processExitedForDrain = $process.HasExited
            if (-not $processExitedForDrain) {
                $processExitedForDrain = $process.WaitForExit(5000)
            }
            if ($processExitedForDrain) {
                # The parameterless overload only flushes the already-ended
                # process' redirected stream bookkeeping; it cannot wait on a
                # live child after the bounded check above succeeded.
                $process.WaitForExit()
            } else {
                $monitorErrors +=
                    'Process remained alive after bounded post-timeout wait; ' +
                    'redirected streams were not drained'
            }
        } catch {
            $monitorErrors += "Bounded WaitForExit: $($_.Exception.Message)"
        }
        if ($processExitedForDrain) {
            try {
                $capturedStdout = $process.S00StdoutTask.GetAwaiter().GetResult()
                [System.IO.File]::WriteAllText(
                    $stdoutLog, $capturedStdout, $script:Utf8NoBom
                )
            } catch {
                $monitorErrors += "Capture stdout: $($_.Exception.Message)"
            }
            try {
                $capturedStderr = $process.S00StderrTask.GetAwaiter().GetResult()
                [System.IO.File]::WriteAllText(
                    $stderrLog, $capturedStderr, $script:Utf8NoBom
                )
            } catch {
                $monitorErrors += "Capture stderr: $($_.Exception.Message)"
            }
        } else {
            [System.IO.File]::WriteAllText($stdoutLog, '', $script:Utf8NoBom)
            [System.IO.File]::WriteAllText($stderrLog, '', $script:Utf8NoBom)
        }
    } else {
        [System.IO.File]::WriteAllText($stdoutLog, '', $script:Utf8NoBom)
        [System.IO.File]::WriteAllText($stderrLog, '', $script:Utf8NoBom)
    }

    $endedUtc = [DateTime]::UtcNow
    $exitCode = $null
    if ($null -ne $process -and $process.HasExited) {
        try {
            $exitCode = $process.ExitCode
        } catch {
            $monitorErrors += "ExitCode: $($_.Exception.Message)"
        }
    }
    $appText = Read-SharedText -Path $appLog
    $stdoutText = Read-SharedText -Path $stdoutLog
    $stderrText = Read-SharedText -Path $stderrLog
    $scanText = $appText + "`n" + $stdoutText + "`n" + $stderrText
    $forbiddenMatches = @(Find-ForbiddenLogMatches -Text $scanText `
        -Patterns @($GateSpec.forbidden_log_patterns_case_insensitive))

    $inputAfter = [ordered]@{}
    $sourceHashesUnchanged = $true
    foreach ($name in $inputBefore.Keys) {
        $after = Get-Sha256Record -Path $inputBefore[$name].path
        $inputAfter[$name] = $after
        if (-not $after.exists -or `
            $after.sha256 -ne $inputBefore[$name].sha256) {
            $sourceHashesUnchanged = $false
        }
    }

    $caseMetrics = [ordered]@{}
    $casePassed = $false
    $copiedOutputPath = $null
    if ($Case -eq 'HotImport') {
        $requiredEventResults = [ordered]@{}
        foreach ($eventValue in @($GateSpec.s00_a_hot_import.required_events)) {
            $event = [string]$eventValue
            $requiredEventResults[$event] = $appText.IndexOf(
                $event, [System.StringComparison]::Ordinal
            ) -ge 0
        }
        $heartbeat = Get-HeartbeatMetrics -LogText $appText
        $commitMatch = [regex]::Match(
            $appText,
            'S00_IMPORT_COMMIT[^\r\n]*duration_seconds=(?<duration>[0-9.]+)' + `
            '[^\r\n]*generation=(?<generation>\d+)' + `
            '[^\r\n]*bones=(?<bones>\d+)[^\r\n]*cubes=(?<cubes>\d+)'
        )
        $dialogMatch = [regex]::Match(
            $appText,
            'S00_DIALOG_CYCLE_END[^\r\n]*requested_hold_ms=(?<requested>\d+)' + `
            '[^\r\n]*actual_hold_ms=(?<actual>\d+)'
        )
        $fgRequested = $childEnvironment.Contains(
            'XPBD_PT_FRAME_GENERATION'
        ) -and [string]$childEnvironment['XPBD_PT_FRAME_GENERATION'] -match `
            '^(?i:1|true)$'
        $fgArmedMatch = [regex]::Match(
            $appText,
            'S00_HOT_IMPORT_ARMED[^\r\n]*required_fg_active_frames=' +
            '(?<required_frames>\d+)[^\r\n]*fg_required=(?<required>[01])'
        )
        $postImportMatch = [regex]::Match(
            $appText,
            'S00_POST_IMPORT_PRESENT[^\r\n]*fg_streak=(?<streak>\d+)'
        )
        $fgHeartbeatMatches = [regex]::Matches(
            $appText,
            'S00_PRESENT_HEARTBEAT[^\r\n]*fg_required=(?<required>[01])' +
            '[^\r\n]*fg_active=(?<active>[01])' +
            '[^\r\n]*fg_presented=(?<presented>\d+)' +
            '[^\r\n]*fg_streak=(?<streak>\d+)'
        )
        $maximumFgPresented = 0
        $fgActivePresentedHeartbeatCount = 0
        foreach ($fgHeartbeatMatch in $fgHeartbeatMatches) {
            $presented = [int]$fgHeartbeatMatch.Groups['presented'].Value
            if ($presented -gt $maximumFgPresented) {
                $maximumFgPresented = $presented
            }
            if ($fgHeartbeatMatch.Groups['active'].Value -eq '1' -and
                $presented -gt 1) {
                ++$fgActivePresentedHeartbeatCount
            }
        }
        $cubeCount = if ($commitMatch.Success) {
            [int]$commitMatch.Groups['cubes'].Value
        } else { -1 }
        $durationSeconds = if ($commitMatch.Success) {
            [double]::Parse(
                $commitMatch.Groups['duration'].Value,
                [System.Globalization.CultureInfo]::InvariantCulture
            )
        } else { $null }
        $durationLimit = Get-ImportDurationLimit -CubeCount $cubeCount `
            -GateSpec $GateSpec -Override $ImportTimeoutSeconds
        $actualHold = if ($dialogMatch.Success) {
            [int]$dialogMatch.Groups['actual'].Value
        } else { $null }
        $allEvents = @($requiredEventResults.Values | Where-Object {
            -not $_
        }).Count -eq 0
        $heartbeatWithinLimit = $heartbeat.count -gt 1 -and `
            $heartbeat.maximum_gap_seconds -le `
            [double]$GateSpec.timeouts.maximum_present_heartbeat_gap_seconds
        $durationWithinLimit = $null -ne $durationSeconds -and `
            $durationSeconds -le $durationLimit
        $dialogHoldSatisfied = $null -ne $actualHold -and `
            $actualHold -ge $DialogHoldMilliseconds
        $cleanExitPassed = $appText -match `
            'S00_CLEAN_EXIT result=passed exit_code=0'
        $fgGateFailureAbsent = $appText -notmatch 'S00_FG_GATE_FAILED'
        $requiredFgFrames = if ($fgArmedMatch.Success) {
            [int]$fgArmedMatch.Groups['required_frames'].Value
        } else { $null }
        $postImportFgStreak = if ($postImportMatch.Success) {
            [int]$postImportMatch.Groups['streak'].Value
        } else { $null }
        $fgContractSatisfied = if ($fgRequested) {
            $fgArmedMatch.Success -and
            $fgArmedMatch.Groups['required'].Value -eq '1' -and
            $null -ne $postImportFgStreak -and
            $null -ne $requiredFgFrames -and
            $postImportFgStreak -ge $requiredFgFrames -and
            $fgActivePresentedHeartbeatCount -gt 0 -and
            $maximumFgPresented -gt 1 -and $fgGateFailureAbsent
        } else {
            $fgArmedMatch.Success -and
            $fgArmedMatch.Groups['required'].Value -eq '0' -and
            $fgGateFailureAbsent
        }
        $caseMetrics = [ordered]@{
            required_events = $requiredEventResults
            all_required_events_observed = $allEvents
            heartbeat = $heartbeat
            heartbeat_limit_seconds = `
                [double]$GateSpec.timeouts.maximum_present_heartbeat_gap_seconds
            heartbeat_within_limit = $heartbeatWithinLimit
            import = [ordered]@{
                commit_parsed = $commitMatch.Success
                duration_seconds = $durationSeconds
                duration_limit_seconds = $durationLimit
                duration_within_limit = $durationWithinLimit
                generation = if ($commitMatch.Success) {
                    [long]$commitMatch.Groups['generation'].Value
                } else { $null }
                bones = if ($commitMatch.Success) {
                    [int]$commitMatch.Groups['bones'].Value
                } else { $null }
                cubes = $cubeCount
            }
            dialog = [ordered]@{
                requested_hold_milliseconds = $DialogHoldMilliseconds
                actual_hold_milliseconds = $actualHold
                hold_satisfied = $dialogHoldSatisfied
            }
            frame_generation_contract = [ordered]@{
                requested_by_environment = $fgRequested
                armed_sentinel_parsed = $fgArmedMatch.Success
                app_fg_required = if ($fgArmedMatch.Success) {
                    $fgArmedMatch.Groups['required'].Value -eq '1'
                } else { $null }
                required_active_frames = $requiredFgFrames
                post_import_active_streak = $postImportFgStreak
                heartbeat_count = $fgHeartbeatMatches.Count
                active_and_presented_heartbeat_count = `
                    $fgActivePresentedHeartbeatCount
                maximum_frames_actually_presented = $maximumFgPresented
                gate_failure_absent = $fgGateFailureAbsent
                satisfied = $fgContractSatisfied
            }
            clean_exit_event_passed = $cleanExitPassed
        }
        $casePassed = $allEvents -and $heartbeatWithinLimit -and `
            $durationWithinLimit -and $dialogHoldSatisfied -and `
            $fgContractSatisfied -and $cleanExitPassed
    } elseif ($Case -eq 'Still') {
        $queueMatches = [regex]::Matches(
            $appText, '(?m)STILL_JOB queue job_id=(?<id>\d+)'
        )
        $jobId = if ($queueMatches.Count -gt 0) {
            [long]$queueMatches[0].Groups['id'].Value
        } else { $null }
        $lifecycle = [ordered]@{}
        foreach ($eventValue in @(
            $GateSpec.s00_d_still_snapshot.required_lifecycle_events
        )) {
            $event = [string]$eventValue
            if ($null -eq $jobId) {
                $lifecycle[$event] = $false
                continue
            }
            $escapedId = [regex]::Escape([string]$jobId)
            $pattern = switch ($event) {
                'queue' { "STILL_JOB queue job_id=$escapedId(?:\s|$)" }
                'begin' { "STILL_JOB begin job_id=$escapedId(?:\s|$)" }
                'progress' { "STILL_JOB progress job_id=$escapedId(?:\s|$)" }
                'readback' { "STILL_JOB readback job_id=$escapedId(?:\s|$)" }
                'save' { "STILL_JOB save job_id=$escapedId(?:\s|$)" }
                'complete' { "STILL_JOB complete job_id=$escapedId(?:\s|$)" }
                default { "STILL_JOB " + [regex]::Escape($event) + `
                    " job_id=$escapedId(?:\s|$)" }
            }
            $lifecycle[$event] = [regex]::IsMatch($appText, $pattern)
        }
        $allLifecycle = @($lifecycle.Values | Where-Object {
            -not $_
        }).Count -eq 0
        $stillExitArmed = $appText -match `
            'S00_STILL_EXIT_ARMED expected_cancel=0'
        $stillCleanExit = if ($null -ne $jobId) {
            [regex]::IsMatch(
                $appText,
                'S00_STILL_CLEAN_EXIT result=completed job_id=' +
                [regex]::Escape([string]$jobId) + '(?:\s|$)'
            )
        } else { $false }
        $stillFailedExitAbsent = $appText -notmatch 'S00_STILL_FAILED_EXIT'
        $completeMatch = [regex]::Match(
            $appText,
            '(?m)STILL_JOB complete job_id=\d+[^\r\n]*path=(?<path>[^\r\n]+)'
        )
        $originalOutputPath = if ($completeMatch.Success) {
            $completeMatch.Groups['path'].Value.Trim()
        } else { $null }
        if (-not [string]::IsNullOrWhiteSpace($originalOutputPath) -and `
            -not [System.IO.Path]::IsPathRooted($originalOutputPath)) {
            $originalOutputPath = [System.IO.Path]::GetFullPath(
                (Join-Path $ApplicationDirectory $originalOutputPath)
            )
        }
        $outputRecord = if (-not [string]::IsNullOrWhiteSpace(
                $originalOutputPath
            )) {
            Get-Sha256Record -Path $originalOutputPath
        } else {
            [ordered]@{
                path = $null
                exists = $false
                bytes = $null
                sha256 = $null
            }
        }
        $signature = [ordered]@{
            extension = $null
            signature_valid = $false
            signature_hex = ''
            validation = 'output missing'
        }
        if ($outputRecord.exists) {
            $signature = Test-ImageSignature -Path $outputRecord.path
            $copiedOutputPath = Join-Path $RunDirectory `
                ([System.IO.Path]::GetFileName($outputRecord.path))
            Copy-Item -LiteralPath $outputRecord.path `
                -Destination $copiedOutputPath
        }
        $caseMetrics = [ordered]@{
            job_id = $jobId
            queue_count = $queueMatches.Count
            lifecycle = $lifecycle
            all_required_lifecycle_observed = $allLifecycle
            still_complete_observed_by_watchdog = $stillCompleteObserved
            s00_exit_hook = [ordered]@{
                armed = $stillExitArmed
                clean_exit_observed = $stillCleanExit
                failed_exit_absent = $stillFailedExitAbsent
            }
            output = $outputRecord
            copied_output_path = $copiedOutputPath
            image_signature = $signature
            graceful_close = [ordered]@{
                disabled = [bool]$LeaveStillWindowOpen
                attempted = $closeAttempted
                accepted = $closeRequested
                grace_hint_seconds = $GracefulCloseSeconds
            }
        }
        $casePassed = $allLifecycle -and $outputRecord.exists -and `
            $outputRecord.bytes -gt 0 -and $signature.signature_valid -and
            $stillExitArmed -and $stillCleanExit -and $stillFailedExitAbsent
    } else {
        $armed = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_ARMED'
        $baselineHeartbeat = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_HEARTBEAT' `
            -RequiredStage 'await_rt_ready'
        $hideCommit = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_HIDE_COMMIT'
        $hiddenObserved = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_HIDDEN_OBSERVED'
        $hiddenHeartbeat = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_HEARTBEAT' -RequiredStage 'hidden'
        $restoreCommit = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_RESTORE_COMMIT'
        $restoredObserved = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_RESTORED_OBSERVED'
        $restoredHeartbeat = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_HEARTBEAT' -RequiredStage 'restored'
        $complete = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_COMPLETE'
        $cleanExit = Get-LastSentinelRecord -LogText $appText `
            -Sentinel 'S00_VISIBILITY_CLEAN_EXIT'

        $eventResults = [ordered]@{
            S00_VISIBILITY_ARMED = $armed.found
            S00_VISIBILITY_HIDE_COMMIT = $hideCommit.found
            S00_VISIBILITY_HIDDEN_OBSERVED = $hiddenObserved.found
            S00_VISIBILITY_RESTORE_COMMIT = $restoreCommit.found
            S00_VISIBILITY_RESTORED_OBSERVED = $restoredObserved.found
            S00_VISIBILITY_COMPLETE = $complete.found
            S00_VISIBILITY_CLEAN_EXIT = $cleanExit.found
        }
        $allVisibilityEvents = @($eventResults.Values | Where-Object {
            -not $_
        }).Count -eq 0
        $visibilityFailureAbsent = $appText -notmatch `
            'S00_VISIBILITY_FAILED'
        $targetMatches = $armed.found -and $complete.found -and `
            $cleanExit.found -and $armed.fields.Contains('target') -and `
            $complete.fields.Contains('target') -and `
            $cleanExit.fields.Contains('target') -and `
            [string]$armed.fields['target'] -eq $ResolvedVisibilityTarget -and `
            [string]$complete.fields['target'] -eq $ResolvedVisibilityTarget -and `
            [string]$cleanExit.fields['target'] -eq $ResolvedVisibilityTarget
        $cleanExitPassed = $cleanExit.found -and `
            $cleanExit.fields.Contains('result') -and `
            [string]$cleanExit.fields['result'] -eq 'passed' -and `
            (Get-SentinelInteger -Record $cleanExit -Name 'exit_code') -eq 0

        $materialControlRequested = `
            $childEnvironment.Contains('XPBD_TEXTURE') -and `
            $childEnvironment.Contains('XPBD_LABPBR_SPECULAR')
        $armedMaterialControlRequired = Get-SentinelInteger -Record $armed `
            -Name 'material_control_required'
        $armedMaterialControlLoaded = Get-SentinelInteger -Record $armed `
            -Name 'material_control_loaded'

        $baselineVisible = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_visible_masks'
        $baselineHidden = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_hidden_masks'
        $baselineEmitters = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_positive_emitters'
        $baselineHiddenSourceEmitters = Get-SentinelInteger `
            -Record $hideCommit -Name 'baseline_hidden_source_emitters'
        $baselineHiddenPositiveWeight = Get-SentinelInteger `
            -Record $hideCommit -Name 'baseline_hidden_positive_weight'
        $baselineBlendIndices = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_blend_indices'
        $baselineFullBuilds = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_full_builds'
        $baselineTlasUpdates = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_tlas_updates'
        $baselineEmitterRebuilds = Get-SentinelInteger -Record $hideCommit `
            -Name 'baseline_emitter_rebuilds'
        $baselineRefits = Get-SentinelInteger -Record $baselineHeartbeat `
            -Name 'refits'
        $baselineBlas = Get-SentinelInteger -Record $baselineHeartbeat `
            -Name 'blas'
        $baselineTlas = Get-SentinelInteger -Record $baselineHeartbeat `
            -Name 'tlas'

        $hiddenVisible = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'visible_masks'
        $hiddenHidden = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'hidden_masks'
        $hiddenEmitters = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'positive_emitters'
        $hiddenSourceEmitters = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'hidden_source_emitters'
        $hiddenPositiveWeight = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'hidden_positive_weight'
        $hiddenBlendIndices = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'blend_indices'
        $hiddenFullBuilds = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'full_builds'
        $hiddenTlasUpdates = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'tlas_updates'
        $hiddenEmitterRebuilds = Get-SentinelInteger -Record $hiddenObserved `
            -Name 'emitter_rebuilds'
        $hiddenRefits = Get-SentinelInteger -Record $hiddenHeartbeat `
            -Name 'refits'

        $restoredVisible = Get-SentinelInteger -Record $restoredObserved `
            -Name 'visible_masks'
        $restoredHidden = Get-SentinelInteger -Record $restoredObserved `
            -Name 'hidden_masks'
        $restoredEmitters = Get-SentinelInteger -Record $restoredObserved `
            -Name 'positive_emitters'
        $restoredHiddenSourceEmitters = Get-SentinelInteger `
            -Record $restoredObserved -Name 'hidden_source_emitters'
        $restoredHiddenPositiveWeight = Get-SentinelInteger `
            -Record $restoredObserved -Name 'hidden_positive_weight'
        $restoredBlendIndices = Get-SentinelInteger -Record $restoredObserved `
            -Name 'blend_indices'
        $restoredFullBuilds = Get-SentinelInteger -Record $restoredObserved `
            -Name 'full_builds'
        $restoredTlasUpdates = Get-SentinelInteger -Record $restoredObserved `
            -Name 'tlas_updates'
        $restoredEmitterRebuilds = Get-SentinelInteger -Record `
            $restoredObserved -Name 'emitter_rebuilds'
        $restoredRefits = Get-SentinelInteger -Record $restoredHeartbeat `
            -Name 'refits'

        $completeFullBuilds = Get-SentinelInteger -Record $complete `
            -Name 'full_builds'
        $completeRefits = Get-SentinelInteger -Record $complete -Name 'refits'
        $completeBlas = Get-SentinelInteger -Record $complete -Name 'blas'
        $completeTlas = Get-SentinelInteger -Record $complete -Name 'tlas'
        $completeTlasUpdates = Get-SentinelInteger -Record $complete `
            -Name 'tlas_updates'
        $completeEmitterRebuilds = Get-SentinelInteger -Record $complete `
            -Name 'emitter_rebuilds'
        $completeHiddenSourceEmitters = Get-SentinelInteger -Record $complete `
            -Name 'hidden_source_emitters'
        $completeHiddenPositiveWeight = Get-SentinelInteger -Record $complete `
            -Name 'hidden_positive_weight'
        $completeBlendIndices = Get-SentinelInteger -Record $complete `
            -Name 'blend_indices'

        $counterValues = @(
            $baselineVisible, $baselineHidden, $baselineEmitters,
            $baselineHiddenSourceEmitters, $baselineHiddenPositiveWeight,
            $baselineBlendIndices,
            $baselineFullBuilds, $baselineTlasUpdates,
            $baselineEmitterRebuilds, $baselineRefits, $baselineBlas,
            $baselineTlas, $hiddenVisible, $hiddenHidden, $hiddenEmitters,
            $hiddenSourceEmitters, $hiddenPositiveWeight, $hiddenBlendIndices,
            $hiddenFullBuilds, $hiddenTlasUpdates,
            $hiddenEmitterRebuilds, $hiddenRefits, $restoredVisible,
            $restoredHidden, $restoredEmitters,
            $restoredHiddenSourceEmitters, $restoredHiddenPositiveWeight,
            $restoredBlendIndices, $restoredFullBuilds,
            $restoredTlasUpdates, $restoredEmitterRebuilds,
            $restoredRefits, $completeFullBuilds, $completeRefits,
            $completeBlas, $completeTlas, $completeTlasUpdates,
            $completeEmitterRebuilds, $completeHiddenSourceEmitters,
            $completeHiddenPositiveWeight, $completeBlendIndices
        )
        $allCountersPresent = $true
        foreach ($counterValue in $counterValues) {
            if ($null -eq $counterValue) {
                $allCountersPresent = $false
                break
            }
        }
        $maskContract = $allCountersPresent -and `
            $baselineVisible + $baselineHidden -eq `
                $hiddenVisible + $hiddenHidden -and `
            $hiddenHidden -gt $baselineHidden -and `
            $hiddenVisible -lt $baselineVisible -and `
            $restoredVisible -eq $baselineVisible -and `
            $restoredHidden -eq $baselineHidden
        $emitterContract = $allCountersPresent -and `
            $hiddenEmitters -le $baselineEmitters -and `
            $restoredEmitters -eq $baselineEmitters
        $requiredHiddenSourceMinimum = [int]`
            $GateSpec.s00_c_pt_visibility.required_outcomes.`
                hidden_source_emitter_triangle_count_minimum
        $requiredHiddenPositiveWeight = [int]`
            $GateSpec.s00_c_pt_visibility.required_outcomes.`
                hidden_final_positive_weight_triangle_count
        $materialControlContract = if (-not $materialControlRequested) {
            $armedMaterialControlRequired -eq 0
        } else {
            $armedMaterialControlRequired -eq 1 -and `
                $armedMaterialControlLoaded -eq 1 -and `
                $baselineBlendIndices -gt 0 -and `
                $baselineEmitters -gt 0 -and `
                $baselineHiddenSourceEmitters -eq 0 -and `
                $baselineHiddenPositiveWeight -eq 0 -and `
                $hiddenBlendIndices -eq $baselineBlendIndices -and `
                $hiddenSourceEmitters -ge $requiredHiddenSourceMinimum -and `
                $hiddenPositiveWeight -eq $requiredHiddenPositiveWeight -and `
                $restoredBlendIndices -eq $baselineBlendIndices -and `
                $restoredHiddenSourceEmitters -eq 0 -and `
                $restoredHiddenPositiveWeight -eq 0 -and `
                $completeBlendIndices -eq $baselineBlendIndices -and `
                $completeHiddenSourceEmitters -eq 0 -and `
                $completeHiddenPositiveWeight -eq 0
        }
        $blasContract = $allCountersPresent -and `
            $hiddenFullBuilds -eq $baselineFullBuilds -and `
            $restoredFullBuilds -eq $baselineFullBuilds -and `
            $completeFullBuilds -eq $baselineFullBuilds -and `
            $hiddenRefits -eq $baselineRefits -and `
            $restoredRefits -eq $baselineRefits -and `
            $completeRefits -eq $baselineRefits -and `
            $completeBlas -eq $baselineBlas -and `
            $completeTlas -eq $baselineTlas
        $tlasUpdateContract = $allCountersPresent -and `
            $hiddenTlasUpdates -gt $baselineTlasUpdates -and `
            $restoredTlasUpdates -gt $hiddenTlasUpdates -and `
            $completeTlasUpdates -ge $restoredTlasUpdates
        $emitterRebuildContract = $allCountersPresent -and `
            $hiddenEmitterRebuilds -gt $baselineEmitterRebuilds -and `
            $restoredEmitterRebuilds -gt $hiddenEmitterRebuilds -and `
            $completeEmitterRebuilds -ge $restoredEmitterRebuilds

        $armedModelGeneration = Get-SentinelInteger -Record $armed `
            -Name 'model_generation'
        $hideModelGeneration = Get-SentinelInteger -Record $hideCommit `
            -Name 'model_generation'
        $completeModelGeneration = Get-SentinelInteger -Record $complete `
            -Name 'model_generation'
        $hideGenerationBefore = Get-SentinelInteger -Record $hideCommit `
            -Name 'visibility_generation_before'
        $hideGenerationAfter = Get-SentinelInteger -Record $hideCommit `
            -Name 'visibility_generation_after'
        $restoreGenerationBefore = Get-SentinelInteger -Record $restoreCommit `
            -Name 'visibility_generation_before'
        $restoreGenerationAfter = Get-SentinelInteger -Record $restoreCommit `
            -Name 'visibility_generation_after'
        $completeVisibilityGeneration = Get-SentinelInteger -Record $complete `
            -Name 'visibility_generation'
        $generationContract = $null -ne $armedModelGeneration -and `
            $null -ne $hideModelGeneration -and `
            $null -ne $completeModelGeneration -and `
            $null -ne $hideGenerationBefore -and `
            $null -ne $hideGenerationAfter -and `
            $null -ne $restoreGenerationBefore -and `
            $null -ne $restoreGenerationAfter -and `
            $null -ne $completeVisibilityGeneration -and `
            $armedModelGeneration -eq $hideModelGeneration -and `
            $hideModelGeneration -eq $completeModelGeneration -and `
            $hideGenerationAfter -gt $hideGenerationBefore -and `
            $restoreGenerationBefore -eq $hideGenerationAfter -and `
            $restoreGenerationAfter -gt $restoreGenerationBefore -and `
            $completeVisibilityGeneration -eq $restoreGenerationAfter

        $caseMetrics = [ordered]@{
            target = $ResolvedVisibilityTarget
            required_events = $eventResults
            all_required_events_observed = $allVisibilityEvents
            failure_sentinel_absent = $visibilityFailureAbsent
            target_matches = $targetMatches
            clean_exit_passed = $cleanExitPassed
            all_counters_present = $allCountersPresent
            mask_contract_passed = $maskContract
            emitter_contract_passed = $emitterContract
            material_control_requested = $materialControlRequested
            material_control_contract_passed = $materialControlContract
            material_control = [ordered]@{
                armed_required = $armedMaterialControlRequired
                armed_loaded = $armedMaterialControlLoaded
                baseline_blend_indices = $baselineBlendIndices
                baseline_positive_emitters = $baselineEmitters
                hidden_source_emitters = $hiddenSourceEmitters
                hidden_positive_weight = $hiddenPositiveWeight
                restored_hidden_source_emitters = `
                    $restoredHiddenSourceEmitters
                restored_hidden_positive_weight = `
                    $restoredHiddenPositiveWeight
                required_hidden_source_minimum = `
                    $requiredHiddenSourceMinimum
                required_hidden_positive_weight = `
                    $requiredHiddenPositiveWeight
            }
            blas_build_and_refit_contract_passed = $blasContract
            tlas_update_contract_passed = $tlasUpdateContract
            emitter_rebuild_contract_passed = $emitterRebuildContract
            generation_contract_passed = $generationContract
            sentinels = [ordered]@{
                armed = $armed
                baseline_heartbeat = $baselineHeartbeat
                hide_commit = $hideCommit
                hidden_observed = $hiddenObserved
                hidden_heartbeat = $hiddenHeartbeat
                restore_commit = $restoreCommit
                restored_observed = $restoredObserved
                restored_heartbeat = $restoredHeartbeat
                complete = $complete
                clean_exit = $cleanExit
            }
        }
        $casePassed = $allVisibilityEvents -and $visibilityFailureAbsent -and `
            $targetMatches -and $cleanExitPassed -and $maskContract -and `
            $emitterContract -and $materialControlContract -and `
            $blasContract -and $tlasUpdateContract -and `
            $emitterRebuildContract -and $generationContract
    }

    $windowCaptureSatisfied = if (-not $CaptureWindow) {
        $true
    } elseif ($Case -eq 'Visibility') {
        $visibilityCapturePhases.baseline -and `
            $visibilityCapturePhases.hidden -and `
            $visibilityCapturePhases.restored
    } else {
        $windowCaptureSuccessCount -gt 0
    }
    $casePassed = $casePassed -and $windowCaptureSatisfied
    $normalExit = $null -ne $exitCode -and $exitCode -eq 0 -and `
        -not $timedOut -and -not $forcedTermination
    $passed = $casePassed -and $normalExit -and `
        $forbiddenMatches.Count -eq 0 -and $sourceHashesUnchanged -and `
        $null -eq $launchError

    $processRecord = [ordered]@{
        schema_version = 1
        exact_pid_watchdog = $true
        pid = if ($null -ne $process) { $process.Id } else { $null }
        state = if ($null -ne $launchError) {
            'LaunchFailed'
        } elseif ($timedOut) {
            'TimedOutAndTerminated'
        } else {
            'Exited'
        }
        started_utc = $startedUtc.ToString('o')
        ended_utc = $endedUtc.ToString('o')
        wall_seconds = ($endedUtc - $startedUtc).TotalSeconds
        wall_timeout_seconds = $TimeoutSeconds
        exit_code = $exitCode
        timed_out = $timedOut
        timeout_recorded_before_force_termination = `
            $timeoutRecordedBeforeForce
        forced_termination = $forcedTermination
        close_main_window_attempted = $closeAttempted
        close_main_window_accepted = $closeRequested
        launch_error = $launchError
        monitor_errors = $monitorErrors
        result = if ($passed) { 'Passed' } else { 'Failed' }
    }
    Write-JsonFile -Path $processJson -Value $processRecord

    $metrics = [ordered]@{
        schema_version = 1
        gate_version = $GateSpec.gate_version
        case = $Case
        repeat_index = $Index
        result = if ($passed) { 'Passed' } else { 'Failed' }
        normal_exit = $normalExit
        source_sha256_unchanged = $sourceHashesUnchanged
        input_after = $inputAfter
        forbidden_log_matches = $forbiddenMatches
        forbidden_log_match_count = $forbiddenMatches.Count
        app_log_exists = Test-Path -LiteralPath $appLog -PathType Leaf
        app_log_bytes = if (Test-Path -LiteralPath $appLog -PathType Leaf) {
            (Get-Item -LiteralPath $appLog).Length
        } else { 0 }
        stdout_bytes = if (Test-Path -LiteralPath $stdoutLog -PathType Leaf) {
            (Get-Item -LiteralPath $stdoutLog).Length
        } else { 0 }
        stderr_bytes = if (Test-Path -LiteralPath $stderrLog -PathType Leaf) {
            (Get-Item -LiteralPath $stderrLog).Length
        } else { 0 }
        window_capture = [ordered]@{
            enabled = [bool]$CaptureWindow
            exact_pid = if ($null -ne $process) { $process.Id } else { $null }
            initialization_error = $windowCaptureInitializationError
            interval_milliseconds = $WindowCaptureIntervalMilliseconds
            maximum_frames = $MaximumWindowCaptureFrames
            attempt_count = $windowCaptureAttemptCount
            successful_frame_count = $windowCaptureSuccessCount
            visibility_phases = $visibilityCapturePhases
            satisfied = $windowCaptureSatisfied
            frames = $windowCaptureRecords
        }
        case_metrics = $caseMetrics
    }
    Write-JsonFile -Path (Join-Path $RunDirectory 'metrics.json') `
        -Value $metrics

    Add-RunnerLog -Path $runnerLog -Message (
        (
            "RUN_END result={0} exit_code={1} timed_out={2} forced={3} " +
            "forbidden_matches={4} source_hash_unchanged={5} window_frames={6}"
        ) -f $(if ($passed) { 'Passed' } else { 'Failed' }), `
            $exitCode, $timedOut, $forcedTermination, `
            $forbiddenMatches.Count, $sourceHashesUnchanged, `
            $windowCaptureSuccessCount
    )
    $artifactHashes = Get-ArtifactHashes -RunDirectory $RunDirectory
    Write-JsonFile -Path (Join-Path $RunDirectory 'artifact_sha256.json') `
        -Value ([ordered]@{
            schema_version = 1
            generated_utc = [DateTime]::UtcNow.ToString('o')
            files = $artifactHashes
        })

    return [pscustomobject]@{
        repeat_index = $Index
        result = if ($passed) { 'Passed' } else { 'Failed' }
        evidence_directory = $RunDirectory
        exit_code = $exitCode
        timed_out = $timedOut
        forced_termination = $forcedTermination
        copied_output_path = $copiedOutputPath
    }
}

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$resolvedApp = Resolve-RepositoryPath -Path $App -RequireFile
$resolvedSpec = Resolve-RepositoryPath -Path $Spec -RequireFile
$gateSpec = Get-Content -LiteralPath $resolvedSpec -Raw -Encoding UTF8 | `
    ConvertFrom-Json
if ([int]$gateSpec.schema_version -ne 1) {
    throw "Unsupported S00 gate spec schema: $($gateSpec.schema_version)"
}
if ([string]::IsNullOrWhiteSpace([string]$gateSpec.gate_version)) {
    throw "Gate spec has no gate_version"
}

if ($Case -eq 'Validate') {
    [pscustomobject]@{
        runner = $MyInvocation.MyCommand.Path
        runner_status = 'Valid'
        app = $resolvedApp
        spec = $resolvedSpec
        gate_version = $gateSpec.gate_version
        supported_cases = @('HotImport', 'Visibility', 'Still')
        force_termination_policy = `
            'Only after a recorded wall-clock timeout'
    } | ConvertTo-Json -Depth 4
    exit 0
}

$baseEnvironment = Get-ChildEnvironment -Entries $Env `
    -JsonFile $EnvironmentFile
if (-not $baseEnvironment.Contains('XPBD_GFX')) {
    $baseEnvironment['XPBD_GFX'] = 'vulkan'
}
if (-not $baseEnvironment.Contains('XPBD_RT')) {
    $baseEnvironment['XPBD_RT'] = '1'
}
if (-not $baseEnvironment.Contains('XPBD_SHOW_BONES')) {
    $baseEnvironment['XPBD_SHOW_BONES'] = '0'
}
if ($Case -eq 'Visibility') {
    $hasVisibilityBaseTexture = $baseEnvironment.Contains('XPBD_TEXTURE') -and `
        -not [string]::IsNullOrWhiteSpace(
            [string]$baseEnvironment['XPBD_TEXTURE']
        )
    $hasVisibilitySpecular = `
        $baseEnvironment.Contains('XPBD_LABPBR_SPECULAR') -and `
        -not [string]::IsNullOrWhiteSpace(
            [string]$baseEnvironment['XPBD_LABPBR_SPECULAR']
        )
    if ($hasVisibilityBaseTexture -ne $hasVisibilitySpecular) {
        throw 'Visibility material control requires both XPBD_TEXTURE and ' + `
            'XPBD_LABPBR_SPECULAR, or neither.'
    }
    if ($hasVisibilityBaseTexture) {
        $baseEnvironment['XPBD_TEXTURE'] = Resolve-RepositoryPath `
            -Path ([string]$baseEnvironment['XPBD_TEXTURE']) -RequireFile
        $baseEnvironment['XPBD_LABPBR_SPECULAR'] = Resolve-RepositoryPath `
            -Path ([string]$baseEnvironment['XPBD_LABPBR_SPECULAR']) `
            -RequireFile
        if (-not $baseEnvironment.Contains('XPBD_PT_EMISSIVE_SURFACES')) {
            $baseEnvironment['XPBD_PT_EMISSIVE_SURFACES'] = '1'
        }
        if (-not $baseEnvironment.Contains('XPBD_PT_NEE')) {
            $baseEnvironment['XPBD_PT_NEE'] = '1'
        }
    }
}

$resolvedStartupModel = ""
if (-not [string]::IsNullOrWhiteSpace($StartupModel)) {
    $resolvedStartupModel = Resolve-RepositoryPath -Path $StartupModel `
        -RequireFile
} elseif ($baseEnvironment.Contains('XPBD_MODEL')) {
    $resolvedStartupModel = Resolve-RepositoryPath `
        -Path ([string]$baseEnvironment['XPBD_MODEL']) -RequireFile
}
if ([string]::IsNullOrWhiteSpace($resolvedStartupModel)) {
    throw "$Case requires -StartupModel or -Env XPBD_MODEL=<path>"
}

$resolvedHotImportModel = ""
if ($Case -eq 'HotImport') {
    if (-not [string]::IsNullOrWhiteSpace($HotImportModel)) {
        $resolvedHotImportModel = Resolve-RepositoryPath -Path $HotImportModel `
            -RequireFile
    } elseif ($baseEnvironment.Contains('XPBD_S00_HOT_IMPORT_MODEL')) {
        $resolvedHotImportModel = Resolve-RepositoryPath `
            -Path ([string]$baseEnvironment['XPBD_S00_HOT_IMPORT_MODEL']) `
            -RequireFile
    }
    if ([string]::IsNullOrWhiteSpace($resolvedHotImportModel)) {
        throw "HotImport requires -HotImportModel or " + `
            "-Env XPBD_S00_HOT_IMPORT_MODEL=<path>"
    }
}

$resolvedVisibilityTarget = ""
if ($Case -eq 'Visibility') {
    if (-not [string]::IsNullOrWhiteSpace($VisibilityTarget)) {
        $resolvedVisibilityTarget = $VisibilityTarget
    } elseif ($baseEnvironment.Contains('XPBD_S00_VISIBILITY_TARGET')) {
        $resolvedVisibilityTarget = `
            [string]$baseEnvironment['XPBD_S00_VISIBILITY_TARGET']
    }
    if ([string]::IsNullOrWhiteSpace($resolvedVisibilityTarget)) {
        throw "Visibility requires -VisibilityTarget or " + `
            "-Env XPBD_S00_VISIBILITY_TARGET=<bone>"
    }
    if ($CaptureWindow -and $MaximumWindowCaptureFrames -lt 3) {
        throw "Visibility -CaptureWindow requires " + `
            "-MaximumWindowCaptureFrames 3 or greater"
    }
}

$effectiveRepeatCount = $RepeatCount
if ($effectiveRepeatCount -eq 0) {
    $effectiveRepeatCount = if ($Case -eq 'Still') {
        [int]$gateSpec.s00_d_still_snapshot.reliability_repeat_count
    } else { 1 }
}
$effectiveWallTimeout = $WallTimeoutSeconds
if ($effectiveWallTimeout -eq 0) {
    $effectiveWallTimeout = [int]$gateSpec.timeouts.process_wall_clock_seconds
}

if ([string]::IsNullOrWhiteSpace($CaseName)) {
    $CaseName = switch ($Case) {
        'HotImport' { 's00_a_hot_import' }
        'Visibility' { 's00_c_pt_visibility' }
        default { 's00_d_still' }
    }
}
if ($CaseName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    throw "CaseName must contain only A-Z, a-z, 0-9, dot, underscore, or dash"
}

$resolvedEvidenceRoot = Resolve-RepositoryPath -Path $EvidenceRoot
$caseRoot = Join-Path $resolvedEvidenceRoot $CaseName
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$groupDirectory = Join-Path $caseRoot $stamp
if (Test-Path -LiteralPath $groupDirectory) {
    $groupDirectory += '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
}
New-Item -ItemType Directory -Path $groupDirectory | Out-Null

$applicationDirectory = Split-Path -Parent $resolvedApp
$gitMetadata = Get-GitMetadata
$hardwareMetadata = Get-HardwareMetadata
$runtimeDllMetadata = @(Get-RuntimeDllMetadata `
    -AppDirectory $applicationDirectory)
$results = @()
for ($index = 1; $index -le $effectiveRepeatCount; ++$index) {
    $runDirectory = Join-Path $groupDirectory ('run_{0:D3}' -f $index)
    $results += Invoke-SingleRun -Index $index -Total $effectiveRepeatCount `
        -RunDirectory $runDirectory -ApplicationPath $resolvedApp `
        -ApplicationDirectory $applicationDirectory -SpecPath $resolvedSpec `
        -GateSpec $gateSpec -BaseEnvironment $baseEnvironment `
        -GitMetadata $gitMetadata -HardwareMetadata $hardwareMetadata `
        -RuntimeDllMetadata $runtimeDllMetadata `
        -ResolvedStartupModel $resolvedStartupModel `
        -ResolvedHotImportModel $resolvedHotImportModel `
        -ResolvedVisibilityTarget $resolvedVisibilityTarget `
        -TimeoutSeconds $effectiveWallTimeout
}

$allPassed = @($results | Where-Object { $_.result -ne 'Passed' }).Count -eq 0
Write-JsonFile -Path (Join-Path $groupDirectory 'summary.json') `
    -Value ([ordered]@{
        schema_version = 1
        gate_version = $gateSpec.gate_version
        case = $Case
        case_name = $CaseName
        started_directory_stamp_utc = $stamp
        repeat_count = $effectiveRepeatCount
        result = if ($allPassed) { 'Passed' } else { 'Failed' }
        runs = $results
    })

[pscustomobject]@{
    result = if ($allPassed) { 'Passed' } else { 'Failed' }
    case = $Case
    repeat_count = $effectiveRepeatCount
    evidence_directory = $groupDirectory
} | ConvertTo-Json -Depth 5

if (-not $allPassed) {
    exit 1
}
exit 0
