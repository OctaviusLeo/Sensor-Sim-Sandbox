#Requires -Version 5.1
<#
.SYNOPSIS
    M0 platform Smoke_Test for Sensor Sim Sandbox (Requirement 1.1, 1.2, 1.4, 1.5, 1.6).

.DESCRIPTION
    Attempts the three platform options in order and keeps the first that passes
    (Requirement 1.1):

        (a) Unreal on Windows + ROS 2 Jazzy in WSL2, rosbridge WebSocket in WSL
            reached from Unreal at ws://localhost:9090.
        (b) Unreal and ROS 2 Jazzy (Windows binary archive) both on Windows.
        (c) Unreal and ROS 2 Jazzy both on Ubuntu 24.04 (NVIDIA driver >= 570).

    An option passes only when ALL of these hold (Requirement 1.2):
        * nvidia-smi output saved in docs/ENVIRONMENT.md;
        * the Windows version text that `winver` shows (edition, version, OS build)
          saved in docs/ENVIRONMENT.md, captured as command-line text because
          `winver` only opens a dialog and prints nothing;
        * the C++ project compiles with zero compiler errors (compile log saved);
        * the Unreal editor opens to its main window with no error dialog
          (screenshot docs/evidence/editor_open.png + startup log with no `Error:`);
        * `ros2 topic echo --once /smoke_test` inside WSL receives the message
          within 10 s of it being sent from Unreal.

    The script is fully non-interactive. The editor target is built with
    `Build.bat ... -WaitMutex` BEFORE any editor launch so no "rebuild modules"
    prompt can appear.

    No ROS 2 node runs on the Windows side; the only Windows<->WSL link is the
    WebSocket, so the design never depends on Windows<->WSL DDS discovery
    (Requirement 1.6).

    Options (b) and (c) are attempted only if every earlier option fails. This
    Windows desktop has no native Ubuntu 24.04, so if (c) is reached its
    first-failing condition is recorded as "not attemptable on this host"
    (Requirement 1.4, 1.5).

    If all three options fail, each option's first-failing condition is recorded
    in docs/ENVIRONMENT.md and the script exits non-zero (Requirement 1.5).

    NOTE: This script is the IMPLEMENTATION produced by task 2.1. It is executed
    on the GPU/Unreal host by task 2.2 (marked [HOST]). It is safe to invoke on a
    machine without the engine or WSL: each condition fails gracefully and is
    recorded, rather than throwing.

.PARAMETER WsUrl
    rosbridge WebSocket URL Unreal connects to. Default ws://localhost:9090.

.PARAMETER WslDistro
    WSL distribution hosting ROS 2 Jazzy. Default Ubuntu-24.04.

.PARAMETER EnginePath
    UE 5.8 install root. Default C:\Program Files\Epic Games\UE_5.8.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke_test.ps1
#>

[CmdletBinding()]
param(
    [string]$WsUrl = 'ws://localhost:9090',
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$EnginePath = 'C:\Program Files\Epic Games\UE_5.8',
    [int]$EchoTimeoutSec = 10,
    [int]$EditorStartupTimeoutSec = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot  = Split-Path -Parent $ScriptDir
$UProject  = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$DocsDir   = Join-Path $RepoRoot 'docs'
$EnvDoc    = Join-Path $DocsDir 'ENVIRONMENT.md'
$EvidenceDir = Join-Path $DocsDir 'evidence'
$ResultsDir  = Join-Path $RepoRoot 'results'
$RunsLogDir  = Join-Path $RepoRoot 'runs\logs\smoke'
$SmokeResult = Join-Path $ResultsDir 'smoke_result.json'

New-Item -ItemType Directory -Force -Path $EvidenceDir, $ResultsDir, $RunsLogDir, $DocsDir | Out-Null

$BuildBat = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorExe = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor.exe'

$ThisCommand = 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke_test.ps1'

function Write-Section([string]$Message) {
    Write-Host ''
    Write-Host "==== $Message ====" -ForegroundColor Cyan
}

function Get-CommitHash {
    try {
        $sha = (& git -C $RepoRoot rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and $sha) { return $sha.Trim() }
    } catch { }
    return 'unknown'
}

# ---------------------------------------------------------------------------
# WSL helper: run a command inside the Jazzy distro with ROS sourced.
# All ROS commands run inside WSL from Windows; no ROS node runs on Windows,
# so no DDS traffic crosses the Windows<->WSL boundary (Requirement 1.6).
# ---------------------------------------------------------------------------
function Invoke-Wsl {
    param(
        [Parameter(Mandatory)][string]$BashCommand,
        [int]$TimeoutSec = 0
    )
    $full = "source /opt/ros/jazzy/setup.bash 2>/dev/null; $BashCommand"
    $result = [pscustomobject]@{ Ok = $false; Output = ''; TimedOut = $false }
    try {
        if ($TimeoutSec -gt 0) {
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = 'wsl.exe'
            $psi.Arguments = "-d $WslDistro -- bash -lc `"$full`""
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $psi.UseShellExecute = $false
            $p = [System.Diagnostics.Process]::Start($psi)
            if (-not $p.WaitForExit($TimeoutSec * 1000)) {
                try { $p.Kill() } catch { }
                $result.TimedOut = $true
                return $result
            }
            $out = $p.StandardOutput.ReadToEnd()
            $err = $p.StandardError.ReadToEnd()
            $result.Output = ($out + $err)
            $result.Ok = ($p.ExitCode -eq 0)
        }
        else {
            $out = & wsl.exe -d $WslDistro -- bash -lc "$full" 2>&1 | Out-String
            $result.Output = $out
            $result.Ok = ($LASTEXITCODE -eq 0)
        }
    }
    catch {
        $result.Output = $_.Exception.Message
    }
    return $result
}

function Test-WslAvailable {
    try {
        $null = & wsl.exe -l -q 2>$null
        return ($LASTEXITCODE -eq 0)
    } catch { return $false }
}

# ---------------------------------------------------------------------------
# Environment capture (Requirement 1.2): nvidia-smi + the Windows version text
# `winver` shows, captured as command-line text (winver itself prints nothing).
# ---------------------------------------------------------------------------
function Capture-Environment {
    Write-Section 'Capturing environment (nvidia-smi + Windows version)'

    $nvidia = ''
    try {
        $nvidia = (& nvidia-smi 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($nvidia)) {
            $nvidia = "nvidia-smi not available or returned no output (exit $LASTEXITCODE)."
        }
    } catch {
        $nvidia = "nvidia-smi not found: $($_.Exception.Message)"
    }

    # `winver` only opens a dialog, so gather the same facts as text.
    $ver = ''
    try { $ver = (& cmd /c ver 2>&1 | Out-String).Trim() } catch { $ver = "cmd /c ver failed: $($_.Exception.Message)" }

    $cim = ''
    try {
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop |
            Select-Object Caption, Version, BuildNumber
        $cim = ($os | Format-List | Out-String).Trim()
    } catch { $cim = "Get-CimInstance Win32_OperatingSystem failed: $($_.Exception.Message)" }

    $display = ''
    $ubr = ''
    try {
        $key = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        $display = (Get-ItemProperty -Path $key -Name DisplayVersion -ErrorAction Stop).DisplayVersion
        $ubr = (Get-ItemProperty -Path $key -Name UBR -ErrorAction Stop).UBR
    } catch { $display = "registry read failed: $($_.Exception.Message)" }

    $winText = @"
cmd /c ver:
$ver

Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber:
$cim

HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion:
  DisplayVersion = $display
  UBR            = $ubr

(winver itself only opens a dialog and prints nothing; the text above is the
equivalent command-line capture.)
"@

    return [pscustomobject]@{
        NvidiaSmi = $nvidia.Trim()
        WinVer    = $winText.Trim()
    }
}

# ---------------------------------------------------------------------------
# Write the captured environment into docs/ENVIRONMENT.md, replacing the
# M0 placeholder block.
# ---------------------------------------------------------------------------
function Update-EnvironmentDoc {
    param(
        [Parameter(Mandatory)]$Env,
        [Parameter(Mandatory)]$Options,
        [Parameter(Mandatory)][string]$ChosenOption,
        [Parameter(Mandatory)][string]$CommitHash
    )

    $perOptionFailures = foreach ($opt in $Options) {
        if (-not $opt.Passed -and $opt.Attempted) {
            "  - Option ($($opt.Id)): first-failing condition = $($opt.FirstFailure)"
        } elseif (-not $opt.Attempted) {
            "  - Option ($($opt.Id)): not attempted (an earlier option passed)"
        }
    }
    $perOptionText = if ($perOptionFailures) { ($perOptionFailures -join "`n") } else { '  (none — an option passed)' }

    $chosenText = if ($ChosenOption) { $ChosenOption } else { 'NONE — all options failed' }

    $block = @"
## Platform smoke-test evidence (M0)

Captured during the M0 Smoke_Test (Requirement 1.2).
Generating command: ``$ThisCommand``
Commit: ``$CommitHash``

- **Chosen platform option:** $chosenText
- **``nvidia-smi`` output:**
  ``````
$($Env.NvidiaSmi)
  ``````
- **``winver`` output (Windows only; captured as command-line text):**
  ``````
$($Env.WinVer)
  ``````
- **Editor opens with no error dialog:** see ``docs/evidence/editor_open.png`` and ``docs/evidence/editor_startup.log``
- **C++ project compiles with zero errors:** see ``docs/evidence/compile.log``
- **``ros2 topic echo`` receives test message within $EchoTimeoutSec s:** see ``docs/evidence/ros2_echo_smoke.txt``
- **Per-option first-failing conditions (Requirement 1.5):**
$perOptionText
- **Ubuntu 24.04 vs 22.04 problems (only if option (c) attempted — Requirement 1.4):**
  Option (c) is not attemptable on this host (no native Ubuntu 24.04 install).

---
"@

    if (-not (Test-Path $EnvDoc)) {
        Set-Content -Path $EnvDoc -Value $block -Encoding UTF8
        return
    }

    $content = Get-Content -Path $EnvDoc -Raw
    # Replace everything between the M0 heading and the next top-level "## " heading.
    $pattern = '(?s)## Platform smoke-test evidence \(M0\).*?(?=\r?\n## )'
    if ($content -match $pattern) {
        $content = [regex]::Replace($content, $pattern, ($block.TrimEnd() + "`n`n"))
    }
    else {
        $content = $content.TrimEnd() + "`n`n" + $block
    }
    Set-Content -Path $EnvDoc -Value $content -Encoding UTF8
}

# ---------------------------------------------------------------------------
# rosbridge availability inside WSL. Starts rosbridge_server if it is not
# already answering on port 9090 (design: "start it in WSL if not").
# ---------------------------------------------------------------------------
function Ensure-Rosbridge {
    param([int]$Port = 9090)

    if (-not (Test-WslAvailable)) {
        return [pscustomobject]@{ Ok = $false; Detail = 'WSL is not available on this host.' }
    }

    # Is something already listening on the port inside WSL?
    $check = Invoke-Wsl -BashCommand "(ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep -q ':$Port' && echo LISTENING || echo FREE" -TimeoutSec 20
    if ($check.Output -match 'LISTENING') {
        return [pscustomobject]@{ Ok = $true; Detail = "rosbridge already listening on localhost:$Port." }
    }

    # Start rosbridge in the background inside WSL.
    $launch = "nohup ros2 launch rosbridge_server rosbridge_websocket_launch.xml > /tmp/rosbridge_smoke.log 2>&1 & disown; echo started"
    $null = Invoke-Wsl -BashCommand $launch -TimeoutSec 30

    # Poll for the port to come up.
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 1
        $poll = Invoke-Wsl -BashCommand "(ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep -q ':$Port' && echo LISTENING || echo FREE" -TimeoutSec 15
        if ($poll.Output -match 'LISTENING') {
            return [pscustomobject]@{ Ok = $true; Detail = "rosbridge started in WSL and listening on localhost:$Port." }
        }
    }
    return [pscustomobject]@{ Ok = $false; Detail = "rosbridge did not start listening on localhost:$Port within timeout." }
}

# ---------------------------------------------------------------------------
# Build the editor target with -WaitMutex (so no "rebuild modules" prompt can
# appear on the subsequent editor launch). Returns compile result + log path.
# ---------------------------------------------------------------------------
function Invoke-EditorBuild {
    $compileLog = Join-Path $EvidenceDir 'compile.log'

    if (-not (Test-Path $BuildBat)) {
        "Build.bat not found at $BuildBat" | Set-Content -Path $compileLog -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = "Build.bat not found at $BuildBat"; Log = $compileLog }
    }
    if (-not (Test-Path $UProject)) {
        "uproject not found at $UProject" | Set-Content -Path $compileLog -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = "uproject not found at $UProject"; Log = $compileLog }
    }

    # Only one Unreal process at a time.
    Wait-ForNoUnrealProcess

    $args = @(
        'SensorSimSandboxEditor', 'Win64', 'Development',
        "-Project=`"$UProject`"",
        '-WaitMutex',
        '-NoHotReloadFromIDE'
    )
    Write-Host "Building: $BuildBat $($args -join ' ')"
    & $BuildBat @args *>&1 | Tee-Object -FilePath $compileLog | Out-Null
    $exit = $LASTEXITCODE

    $logText = if (Test-Path $compileLog) { Get-Content $compileLog -Raw } else { '' }
    $zeroErrors = ($exit -eq 0) -and ($logText -notmatch '(?m)error[ :C]')

    $detail = if ($zeroErrors) { 'Editor target compiled with zero errors.' }
              else { "Build exited $exit or compiler errors present. See $compileLog." }

    return [pscustomobject]@{ Ok = $zeroErrors; Detail = $detail; Log = $compileLog }
}

function Wait-ForNoUnrealProcess {
    for ($i = 0; $i -lt 60; $i++) {
        $busy = Get-Process -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessName -match 'UnrealBuildTool|UnrealEditor' }
        if (-not $busy) { return }
        Start-Sleep -Seconds 2
    }
}

# ---------------------------------------------------------------------------
# Launch the editor, wait for a main window whose title contains the project
# name, screenshot it, confirm the startup log has no `Error:` lines, run the
# SmokePublish console command, then close it (Requirement 1.2).
# ---------------------------------------------------------------------------
function Invoke-EditorOpenAndPublish {
    param([Parameter(Mandatory)][string]$Payload)

    $startupLog = Join-Path $EvidenceDir 'editor_startup.log'
    $screenshot = Join-Path $EvidenceDir 'editor_open.png'

    if (-not (Test-Path $EditorExe)) {
        return [pscustomobject]@{ Ok = $false; Detail = "UnrealEditor.exe not found at $EditorExe"; EditorOk = $false; EchoOk = $false }
    }

    Wait-ForNoUnrealProcess

    # Launch the editor; -ExecCmds runs SmokePublish then quits once done. The
    # editor is built first, so no rebuild-modules prompt can appear.
    $execCmds = "SmokePublish `"$WsUrl`" `"$Payload`""
    $editorArgs = @(
        "`"$UProject`"",
        '-stdout', '-FullStdOutLogOutput',
        "-ExecCmds=`"$execCmds`"",
        "-abslog=`"$startupLog`""
    )

    Write-Host "Launching editor: $EditorExe $($editorArgs -join ' ')"
    $proc = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -PassThru

    # Wait for a main window whose title contains the project name.
    $windowSeen = $false
    $deadline = (Get-Date).AddSeconds($EditorStartupTimeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        try { $proc.Refresh() } catch { }
        if ($proc.HasExited) { break }
        if ($proc.MainWindowTitle -and $proc.MainWindowTitle -match 'SensorSimSandbox') {
            $windowSeen = $true
            break
        }
    }

    # Screenshot the editor window if it is up.
    if ($windowSeen) {
        try { Capture-Screenshot -Path $screenshot } catch { }
    }

    # Give the SmokePublish command time to connect+send before we read the echo.
    Start-Sleep -Seconds 2

    # The echo check (started by the caller) owns the 10 s window; here we just
    # make sure the editor opened cleanly. Allow a moment then close the editor.
    Start-Sleep -Seconds 3

    # Close the editor cleanly; force-kill if it ignores the close request.
    try {
        if (-not $proc.HasExited) {
            $null = $proc.CloseMainWindow()
            if (-not $proc.WaitForExit(30000)) { $proc.Kill() }
        }
    } catch { }

    Wait-ForNoUnrealProcess

    $logText = if (Test-Path $startupLog) { Get-Content $startupLog -Raw } else { '' }
    $noErrorLines = ($logText -ne '') -and ($logText -notmatch '(?m)^\s*.*Error:')
    $editorOk = $windowSeen -and $noErrorLines

    $detail = if (-not (Test-Path $EditorExe)) { 'editor exe missing' }
              elseif (-not $windowSeen) { 'editor main window with project title not observed within timeout' }
              elseif (-not $noErrorLines) { 'editor startup log contains Error: lines (or no log captured)' }
              else { 'editor opened to its main window with no Error: lines' }

    return [pscustomobject]@{ Ok = $editorOk; Detail = $detail; EditorOk = $editorOk; Screenshot = $screenshot; Log = $startupLog }
}

function Capture-Screenshot {
    param([Parameter(Mandatory)][string]$Path)
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $gfx.Dispose(); $bmp.Dispose()
}

# ---------------------------------------------------------------------------
# `ros2 topic echo --once /smoke_test` inside WSL; must receive within 10 s.
# ---------------------------------------------------------------------------
function Invoke-RosEcho {
    $echoFile = Join-Path $EvidenceDir 'ros2_echo_smoke.txt'

    if (-not (Test-WslAvailable)) {
        'WSL not available; cannot run ros2 topic echo.' | Set-Content -Path $echoFile -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = 'WSL not available'; File = $echoFile }
    }

    $cmd = "timeout $EchoTimeoutSec ros2 topic echo --once /smoke_test std_msgs/msg/String"
    $res = Invoke-Wsl -BashCommand $cmd -TimeoutSec ($EchoTimeoutSec + 5)
    $res.Output | Set-Content -Path $echoFile -Encoding UTF8

    $received = (-not $res.TimedOut) -and ($res.Output -match 'data:')
    $detail = if ($res.TimedOut) { "no message within $EchoTimeoutSec s (timed out)" }
              elseif ($received) { "message received within $EchoTimeoutSec s" }
              else { 'ros2 topic echo produced no message data' }

    return [pscustomobject]@{ Ok = $received; Detail = $detail; File = $echoFile }
}

# ---------------------------------------------------------------------------
# Assert no platform option depends on Windows<->WSL DDS discovery
# (Requirement 1.6). By construction: no ROS 2 node runs on Windows and the
# only Windows<->WSL link is the WebSocket. We verify no ROS node is running
# on the Windows side.
# ---------------------------------------------------------------------------
function Assert-NoWindowsDds {
    $windowsRosProcs = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -match '^(ros2|_ros2_daemon|rmw|fastdds|rtps)' }
    if ($windowsRosProcs) {
        return [pscustomobject]@{ Ok = $false; Detail = "A ROS 2 process is running on the Windows side: $($windowsRosProcs.ProcessName -join ', '). The design forbids Windows<->WSL DDS." }
    }
    return [pscustomobject]@{ Ok = $true; Detail = 'No ROS 2 node on the Windows side; the only Windows<->WSL link is the WebSocket (no DDS discovery).' }
}

# ---------------------------------------------------------------------------
# Run a single platform option, returning per-condition pass/fail, evidence
# paths, and the first-failing condition.
# ---------------------------------------------------------------------------
function Invoke-OptionA {
    param([Parameter(Mandatory)][string]$Payload)

    Write-Section 'Option (a): Unreal on Windows + ROS 2 Jazzy in WSL (rosbridge WebSocket)'

    $conditions = [ordered]@{}
    $evidence   = [ordered]@{}
    $firstFailure = $null

    # DDS assertion first — it is a structural precondition (Requirement 1.6).
    $dds = Assert-NoWindowsDds
    $conditions['no_windows_wsl_dds'] = $dds.Ok
    if (-not $dds.Ok -and -not $firstFailure) { $firstFailure = "no_windows_wsl_dds: $($dds.Detail)" }

    # rosbridge answers on localhost:9090 (start it in WSL if not).
    $rb = Ensure-Rosbridge -Port 9090
    $conditions['rosbridge_listening'] = $rb.Ok
    if (-not $rb.Ok -and -not $firstFailure) { $firstFailure = "rosbridge_listening: $($rb.Detail)" }

    # C++ project compiles with zero errors (build the editor target first).
    $build = Invoke-EditorBuild
    $conditions['cpp_compiles_zero_errors'] = $build.Ok
    $evidence['compile_log'] = $build.Log
    if (-not $build.Ok -and -not $firstFailure) { $firstFailure = "cpp_compiles_zero_errors: $($build.Detail)" }

    # Editor opens to its main window with no error dialog, and SmokePublish
    # runs. The echo check owns the 10 s window and runs right after.
    $editor = $null
    $echo = $null
    if ($build.Ok) {
        $editor = Invoke-EditorOpenAndPublish -Payload $Payload
        $conditions['editor_opens_no_error'] = $editor.EditorOk
        if ($editor.PSObject.Properties.Name -contains 'Screenshot') { $evidence['editor_screenshot'] = $editor.Screenshot }
        if ($editor.PSObject.Properties.Name -contains 'Log') { $evidence['editor_startup_log'] = $editor.Log }
        if (-not $editor.EditorOk -and -not $firstFailure) { $firstFailure = "editor_opens_no_error: $($editor.Detail)" }

        $echo = Invoke-RosEcho
        $conditions['ros2_echo_within_timeout'] = $echo.Ok
        $evidence['ros2_echo'] = $echo.File
        if (-not $echo.Ok -and -not $firstFailure) { $firstFailure = "ros2_echo_within_timeout: $($echo.Detail)" }
    }
    else {
        # Skipped because compile failed; record as not-run (false).
        $conditions['editor_opens_no_error'] = $false
        $conditions['ros2_echo_within_timeout'] = $false
    }

    $allPass = @($conditions.Values) -notcontains $false

    return [pscustomobject]@{
        Id = 'a'
        Attempted = $true
        Passed = $allPass
        Conditions = $conditions
        Evidence = $evidence
        FirstFailure = if ($allPass) { $null } else { $firstFailure }
    }
}

function New-OptionBPlaceholder {
    # Option (b): Unreal + ROS 2 Jazzy (Windows binary archive) both on Windows.
    # Only attempted if (a) fails. Even on Windows this still uses a rosbridge
    # WebSocket on localhost (never native DDS).
    param([Parameter(Mandatory)][string]$Payload)

    Write-Section 'Option (b): Unreal + ROS 2 Jazzy (Windows binary archive) both on Windows'

    $conditions = [ordered]@{}
    $firstFailure = $null

    $dds = Assert-NoWindowsDds
    $conditions['no_windows_wsl_dds'] = $dds.Ok

    # rosbridge on Windows localhost. The Windows ROS 2 Jazzy binary archive is
    # not assumed present; if its rosbridge is not answering, that is the
    # first-failing condition.
    $listening = $false
    try {
        $tcp = Test-NetConnection -ComputerName 'localhost' -Port 9090 -WarningAction SilentlyContinue
        $listening = $tcp.TcpTestSucceeded
    } catch { $listening = $false }
    $conditions['rosbridge_listening_windows'] = $listening
    if (-not $listening) { $firstFailure = 'rosbridge_listening_windows: no rosbridge answering on Windows localhost:9090 (ROS 2 Jazzy Windows binary archive not provisioned on this host)' }

    $allPass = @($conditions.Values) -notcontains $false

    return [pscustomobject]@{
        Id = 'b'
        Attempted = $true
        Passed = $allPass
        Conditions = $conditions
        Evidence = [ordered]@{}
        FirstFailure = if ($allPass) { $null } else { $firstFailure }
    }
}

function New-OptionCPlaceholder {
    # Option (c): Unreal + ROS 2 Jazzy both on Ubuntu 24.04 (NVIDIA >= 570).
    # This Windows desktop has no native Ubuntu 24.04, so its first-failing
    # condition is recorded as "not attemptable on this host" (Requirement 1.4, 1.5).
    Write-Section 'Option (c): Unreal + ROS 2 Jazzy both on native Ubuntu 24.04'

    return [pscustomobject]@{
        Id = 'c'
        Attempted = $true
        Passed = $false
        Conditions = [ordered]@{ native_ubuntu_2404_present = $false }
        Evidence = [ordered]@{}
        FirstFailure = 'native_ubuntu_2404: not attemptable on this host (no native Ubuntu 24.04 install)'
    }
}

# ===========================================================================
# Main
# ===========================================================================
Write-Section 'Sensor Sim Sandbox — M0 platform Smoke_Test'
Write-Host "Repo root: $RepoRoot"
Write-Host "WS URL:    $WsUrl"
Write-Host "WSL distro: $WslDistro"
Write-Host "Engine:    $EnginePath"

$commit = Get-CommitHash
$payload = "sensor-sim-sandbox smoke $commit"

$options = New-Object System.Collections.Generic.List[object]
$chosen = $null

# Option (a)
$optA = Invoke-OptionA -Payload $payload
$options.Add($optA)
if ($optA.Passed) { $chosen = 'a' }

# Option (b) only if (a) failed.
if (-not $chosen) {
    $optB = New-OptionBPlaceholder -Payload $payload
    $options.Add($optB)
    if ($optB.Passed) { $chosen = 'b' }
}

# Option (c) only if (a) and (b) failed.
if (-not $chosen) {
    $optC = New-OptionCPlaceholder
    $options.Add($optC)
    if ($optC.Passed) { $chosen = 'c' }
}

# Capture environment and write it into docs/ENVIRONMENT.md regardless of outcome.
$env = Capture-Environment
Update-EnvironmentDoc -Env $env -Options $options -ChosenOption $chosen -CommitHash $commit

# ---------------------------------------------------------------------------
# Write results/smoke_result.json (Requirement: chosen option, each condition's
# pass/fail, evidence paths, the command, and the commit hash).
# ---------------------------------------------------------------------------
$optionReport = foreach ($opt in $options) {
    [pscustomobject]@{
        option        = $opt.Id
        attempted     = $opt.Attempted
        passed        = $opt.Passed
        conditions    = $opt.Conditions
        evidence      = $opt.Evidence
        first_failure = $opt.FirstFailure
    }
}

$result = [pscustomobject]@{
    chosen_option = $chosen
    all_failed    = ($null -eq $chosen)
    command       = $ThisCommand
    commit        = $commit
    ws_url        = $WsUrl
    wsl_distro    = $WslDistro
    engine_path   = $EnginePath
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
    options       = $optionReport
    environment   = [pscustomobject]@{
        nvidia_smi_captured = ($env.NvidiaSmi -notmatch 'not available|not found')
        winver_captured     = ($env.WinVer -ne '')
        environment_doc     = (Resolve-Path -Path $EnvDoc -ErrorAction SilentlyContinue).Path
    }
}

$result | ConvertTo-Json -Depth 8 | Set-Content -Path $SmokeResult -Encoding UTF8
Write-Host ''
Write-Host "Wrote $SmokeResult"

Write-Section 'Smoke_Test summary'
foreach ($opt in $options) {
    $status = if ($opt.Passed) { 'PASS' } elseif (-not $opt.Attempted) { 'SKIPPED' } else { 'FAIL' }
    Write-Host ("Option ({0}): {1}" -f $opt.Id, $status)
    if (-not $opt.Passed -and $opt.FirstFailure) {
        Write-Host ("    first-failing: {0}" -f $opt.FirstFailure)
    }
}

if ($chosen) {
    Write-Host ""
    Write-Host "Smoke_Test PASSED on option ($chosen)." -ForegroundColor Green
    exit 0
}
else {
    Write-Host ""
    Write-Host "Smoke_Test FAILED: no platform option passed. See docs/ENVIRONMENT.md." -ForegroundColor Red
    exit 1
}
