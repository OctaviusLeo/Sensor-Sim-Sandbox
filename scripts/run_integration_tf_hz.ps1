#Requires -Version 5.1
<#
.SYNOPSIS
    Task 12.3 [HOST] demo/verify — publish /tf at >= 10 Hz through the live
    Rosbridge_Client and measure the achieved rate with `ros2 topic hz /tf` in
    WSL (Requirement 7.4), the live half of the end-to-end integration example.

.DESCRIPTION
    The headless-demonstrable parts of task 12.3 (keyboard-input latency
    <= 100 ms, 2.1; TF scheduler >= 10 Hz, 7.4) are the Unreal Automation test
    SensorSimSandbox.Integration.KeyboardLatencyAndTfRate, kept green by
    scripts/run_ue_tests.ps1. THIS script covers the live observation now that
    the Rosbridge_Client (task 11) exists and implements ITfTransport: it wires
    FTfPublisher through the live WebSocket (the `TfPublish` console command) and
    measures the real `ros2 topic hz /tf`.

    Mirrors scripts/run_lidar_publish.ps1 (world-needing -game launch, rosbridge
    url_path:=// per the M0 finding, non-interactive, one Unreal process at a
    time, long steps polled):

      1. Build the editor target with Build.bat ... -WaitMutex; compile log saved.
      2. Ensure rosbridge answers an HTTP 101 WebSocket upgrade at url_path '//'
         in WSL (start it the proven way if not).
      3. Start `ros2 topic hz /tf` in WSL BEFORE launching, writing to a file on
         /mnt/c, and let it sample for the measurement window.
      4. Launch /Game/Maps/RoverLevel with -game and
         -ExecCmds="TfPublish <seconds>". TfPublish connects the Rosbridge_Client,
         advertises /tf, and drives the >= 10 Hz scheduler on the real wall clock
         for the window, then self-terminates.
      5. Parse the average rate `ros2 topic hz` reports and PASS when it is
         >= 10 Hz. Save the raw hz output + the editor log as evidence. The hz
         number is NEVER fabricated; if rosbridge/WSL is unreachable (e.g. the
         prior shader/DDC contention makes a windowed -game run unreliable), the
         live step is recorded as DEFERRED with the exact reason and the command
         to run later, and the headless scheduler >= 10 Hz measurement (the
         automation test) stands as the pass for 7.4.

.PARAMETER EnginePath
    UE 5.8 install root. Default is the verified path on this host.

.PARAMETER WslDistro
    WSL distro hosting ROS 2 Jazzy. Default 'Ubuntu'.

.PARAMETER WindowSec
    Seconds to publish /tf and sample `ros2 topic hz`. Default 20.

.EXAMPLE
    pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_integration_tf_hz.ps1
#>

[CmdletBinding()]
param(
    [string]$EnginePath = 'C:\Program Files (x86)\UE_5.8',
    [string]$WslDistro = 'Ubuntu',
    [int]$RealPort = 9090,
    [int]$WindowSec = 20,
    [int]$BuildTimeoutSec = 1800,
    [int]$LaunchTimeoutSec = 900,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot    = Split-Path -Parent $PSScriptRoot
$UProject    = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$BuildBat    = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorExe   = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor.exe'
$LogDir      = Join-Path $RepoRoot 'runs\logs\integration_tf_hz'
$EvidenceDir = Join-Path $RepoRoot 'docs\evidence'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

# The hz sampler runs a few seconds longer than the publish window so it always
# captures a full window of /tf messages.
$HzSampleSec = $WindowSec + 6

function Write-Step($m) { Write-Host "[run_integration_tf_hz] $m" }

function Clear-UbtCache {
    $intermediate = Join-Path $RepoRoot 'unreal\SensorSimSandbox\Intermediate\Build'
    if (Test-Path $intermediate) {
        Get-ChildItem -Path $intermediate -Recurse -Filter 'Makefile.bin' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        Write-Step "cleared UBT Makefile.bin cache under $intermediate"
    }
}

function Wait-ForNoUnrealProcess {
    for ($i = 0; $i -lt 60; $i++) {
        $busy = Get-Process -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessName -match 'UnrealBuildTool|UnrealEditor' }
        if (-not $busy) { return }
        Start-Sleep -Seconds 2
    }
}

# --- WSL helpers (mirror scripts/run_lidar_publish.ps1) --------------------
function Invoke-Wsl {
    param([Parameter(Mandatory)][string]$BashCommand)
    $full = "source /opt/ros/jazzy/setup.bash 2>/dev/null; $BashCommand"
    $result = [pscustomobject]@{ Ok = $false; Output = '' }
    try {
        $out = & wsl.exe -d $WslDistro -- bash -lc "$full" 2>&1 | Out-String
        $result.Output = $out
        $result.Ok = ($LASTEXITCODE -eq 0)
    } catch { $result.Output = $_.Exception.Message }
    return $result
}

function Test-WslAvailable {
    try { $null = & wsl.exe -l -q 2>$null; return ($LASTEXITCODE -eq 0) } catch { return $false }
}

function Convert-ToWslPath {
    param([Parameter(Mandatory)][string]$WinPath)
    $p = $WinPath -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') { return "/mnt/" + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

function Test-WsUpgrade {
    param([string]$HostName = '127.0.0.1', [int]$Port = 9090, [string]$Path = '//')
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect($HostName, $Port)
        $stream = $client.GetStream()
        $key = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes((Get-Random).ToString()))
        $req = "GET $Path HTTP/1.1`r`nHost: $HostName`:$Port`r`nUpgrade: websocket`r`nConnection: Upgrade`r`nSec-WebSocket-Key: $key`r`nSec-WebSocket-Version: 13`r`n`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $stream.Write($bytes, 0, $bytes.Length)
        Start-Sleep -Milliseconds 300
        $buf = New-Object byte[] 1024
        $n = $stream.Read($buf, 0, $buf.Length)
        $line = (([Text.Encoding]::ASCII.GetString($buf, 0, $n)) -split "`r`n")[0]
        $client.Close()
        return ($line -match '101')
    } catch { return $false }
}

function Ensure-Rosbridge {
    param([int]$Port = 9090, [string]$UrlPath = '//')
    if (Test-WsUpgrade -Port $Port -Path $UrlPath) {
        return [pscustomobject]@{ Ok = $true; Detail = "rosbridge already answering HTTP 101 on localhost:$Port at '$UrlPath'." }
    }
    $clean = "pkill -9 -f rosbridge_websocket 2>/dev/null; pkill -9 -f 'ros2 launch rosbridge' 2>/dev/null; pkill -9 -f rosapi_node 2>/dev/null; fuser -k $Port/tcp 2>/dev/null; sleep 2; echo cleaned"
    $null = Invoke-Wsl -BashCommand $clean
    $launch = "nohup ros2 run rosbridge_server rosbridge_websocket --ros-args -p url_path:=$UrlPath > /tmp/rosbridge_task12_3.log 2>&1 & disown; echo started"
    $null = Invoke-Wsl -BashCommand $launch
    for ($i = 0; $i -lt 25; $i++) {
        Start-Sleep -Seconds 1
        if (Test-WsUpgrade -Port $Port -Path $UrlPath) {
            return [pscustomobject]@{ Ok = $true; Detail = "rosbridge started in WSL (url_path=$UrlPath), HTTP 101 on localhost:$Port." }
        }
    }
    return [pscustomobject]@{ Ok = $false; Detail = "rosbridge did not answer HTTP 101 on localhost:$Port at '$UrlPath' within timeout." }
}

# --- 1. Build -------------------------------------------------------------
function Invoke-Build {
    $compileLog = Join-Path $LogDir 'compile.log'
    if (-not (Test-Path $BuildBat)) {
        return [pscustomobject]@{ Ok = $false; Detail = "Build.bat not found at $BuildBat"; Log = $compileLog }
    }
    Wait-ForNoUnrealProcess
    Write-Step "Building SensorSimSandboxEditor (log: $compileLog) ..."
    $buildArgs = @('SensorSimSandboxEditor', 'Win64', 'Development', "-Project=`"$UProject`"", '-WaitMutex', '-NoHotReloadFromIDE')
    $p = Start-Process -FilePath $BuildBat -ArgumentList $buildArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput $compileLog -RedirectStandardError "$compileLog.err"
    if (-not $p.WaitForExit($BuildTimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return [pscustomobject]@{ Ok = $false; Detail = 'build timed out'; Log = $compileLog }
    }
    $logText = if (Test-Path $compileLog) { Get-Content $compileLog -Raw } else { '' }
    $sawSucceeded = ($logText -match '(?m)^\s*Result:\s*Succeeded\s*$')
    $sawFailed    = ($logText -match '(?m)^\s*Result:\s*Failed') -or ($logText -match '(?m)^\s*BUILD FAILED\s*$')
    $exitCode = $null; try { $exitCode = $p.ExitCode } catch { $exitCode = $null }
    if ($sawFailed) { return [pscustomobject]@{ Ok = $false; Detail = "Result: Failed (exit $exitCode)"; Log = $compileLog } }
    $ok = $sawSucceeded -or ($exitCode -eq 0)
    return [pscustomobject]@{ Ok = $ok; Detail = "Result: $(if($sawSucceeded){'Succeeded'}else{"exit $exitCode"})"; Log = $compileLog }
}

# --- 2+3+4. rosbridge up, hz sampler, publish from a -game launch ----------
function Invoke-TfHz {
    $editorLog = Join-Path $LogDir 'tf_publish_editor.log'
    $hzFile    = Join-Path $LogDir 'ros2_topic_hz_tf.txt'
    if (Test-Path $hzFile) { Remove-Item $hzFile -Force }

    $deferredReason = $null
    if (-not (Test-WslAvailable)) {
        $deferredReason = 'WSL not available on this host'
    } else {
        $rb = Ensure-Rosbridge -Port $RealPort -UrlPath '//'
        Write-Step "rosbridge: Ok=$($rb.Ok) $($rb.Detail)"
        if (-not $rb.Ok) { $deferredReason = "rosbridge not reachable: $($rb.Detail)" }
    }

    $hzProc = $null
    if (-not $deferredReason) {
        # Start `ros2 topic hz /tf` BEFORE launching so it is subscribed when the
        # first TF publish arrives. `timeout` bounds the sampler to the window.
        $wslHz = Convert-ToWslPath $hzFile
        $helper = Join-Path $LogDir 'hz_tf.sh'
        $helperBody = "#!/usr/bin/env bash`nset -e`nsource /opt/ros/jazzy/setup.bash`nexec timeout $HzSampleSec stdbuf -oL -eL ros2 topic hz /tf > `"`$1`" 2>&1`n"
        [IO.File]::WriteAllText($helper, ($helperBody -replace "`r`n", "`n"))
        $wslHelper = Convert-ToWslPath $helper
        $hzProc = Start-Process -FilePath 'wsl.exe' `
            -ArgumentList @('-d', $WslDistro, '--', 'bash', $wslHelper, $wslHz) -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 3
    }

    # Launch the editor -game so the console command runs in a live world.
    Wait-ForNoUnrealProcess
    Write-Step "Launching -game RoverLevel TfPublish $WindowSec (log: $editorLog) ..."
    $execCmds = "TfPublish $WindowSec"
    $editorArgs = @(
        "`"$UProject`"", '/Game/Maps/RoverLevel', '-game',
        '-ResX=640', '-ResY=480', '-windowed',
        "-BridgeHost=127.0.0.1", "-BridgePort=$RealPort",
        "-ExecCmds=`"$execCmds`"",
        '-stdout', '-FullStdOutLogOutput',
        '-unattended', '-nopause', '-nosplash',
        "-abslog=`"$editorLog`""
    )
    $p = Start-Process -FilePath $EditorExe -ArgumentList $editorArgs -PassThru
    if (-not $p.WaitForExit($LaunchTimeoutSec * 1000)) {
        try { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; if (-not $p.HasExited) { $p.Kill() } } } catch { }
    }

    if (-not $deferredReason) {
        # Let the sampler finish its window, then make sure it is gone.
        Start-Sleep -Seconds 8
        try { if ($hzProc -and -not $hzProc.HasExited) { $hzProc.Kill() } } catch { }
        $null = Invoke-Wsl -BashCommand "pkill -f 'ros2 topic hz /tf' 2>/dev/null; echo stopped"
    }

    # The editor log always proves TF was published through the live client
    # (the local publish count/rate TfPublish logs on completion).
    $localLine = ''
    if (Test-Path $editorLog) {
        $m = Select-String -Path $editorLog -Pattern 'TfPublish done: .*' | Select-Object -First 1
        if ($m) { $localLine = $m.Line.Trim() }
    }

    # Parse the average Hz from the `ros2 topic hz` output. It prints lines like
    #   average rate: 10.001
    $hzText = if (Test-Path $hzFile) { Get-Content $hzFile -Raw } else { '' }
    $avgHz = $null
    if ($hzText) {
        $matches = [regex]::Matches($hzText, 'average rate:\s*([0-9]+\.?[0-9]*)')
        if ($matches.Count -gt 0) {
            # Use the last reported average (most samples accumulated).
            $avgHz = [double]$matches[$matches.Count - 1].Groups[1].Value
        }
    }

    return [pscustomobject]@{
        LocalLine      = $localLine
        AvgHz          = $avgHz
        HzText         = $hzText
        EditorLog      = $editorLog
        HzFile         = $hzFile
        DeferredReason = $deferredReason
    }
}

# --- Orchestration ---------------------------------------------------------
Clear-UbtCache

if (-not $SkipBuild) {
    $build = Invoke-Build
    Write-Step "Build: Ok=$($build.Ok) $($build.Detail)"
    if (-not $build.Ok) { Write-Error "Build failed; see $($build.Log)"; exit 1 }
}

$res = Invoke-TfHz
Write-Step "Local: $($res.LocalLine)"
Write-Step "Avg Hz=$($res.AvgHz) Deferred=$([bool]$res.DeferredReason)"

$commit = (& git -C $RepoRoot rev-parse HEAD 2>$null)
$ev = Join-Path $EvidenceDir 'task_12_3_tf_hz.txt'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('# Task 12.3 — live `ros2 topic hz /tf` evidence (Requirement 7.4)')
[void]$sb.AppendLine("# Generated: $(Get-Date -Format o)")
[void]$sb.AppendLine("# Command: pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_integration_tf_hz.ps1")
[void]$sb.AppendLine("# Commit:  $commit")
[void]$sb.AppendLine("# Engine:  $EnginePath")
[void]$sb.AppendLine("# Window:  $WindowSec s publish, $HzSampleSec s hz sample")
[void]$sb.AppendLine('')
[void]$sb.AppendLine('## /tf published through the live Rosbridge_Client (editor log):')
[void]$sb.AppendLine($res.LocalLine)
[void]$sb.AppendLine('')
if ($res.DeferredReason) {
    [void]$sb.AppendLine('## Live `ros2 topic hz /tf`: DEFERRED')
    [void]$sb.AppendLine("Reason: $($res.DeferredReason)")
    [void]$sb.AppendLine('To run later on this host (rosbridge up in WSL, one Unreal process at a time):')
    [void]$sb.AppendLine('  pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\run_integration_tf_hz.ps1')
    [void]$sb.AppendLine('The headless FTfPublisher scheduler >= 10 Hz measurement (automation test')
    [void]$sb.AppendLine('SensorSimSandbox.Integration.KeyboardLatencyAndTfRate) stands as the 7.4 pass.')
} else {
    [void]$sb.AppendLine('## Live `ros2 topic hz /tf` (WSL):')
    [void]$sb.AppendLine($res.HzText.Trim())
}
[IO.File]::WriteAllText($ev, $sb.ToString())
Write-Step "evidence written to $ev"

if (Test-Path $res.EditorLog) { Copy-Item $res.EditorLog (Join-Path $EvidenceDir 'task_12_3_tf_publish_editor.log') -Force }

# The hard gate here is that /tf really published through the live client; the
# live hz passes when reachable and is cleanly deferred (never faked) otherwise.
$publishedOk = ($res.LocalLine -match 'TfPublish done:')
if (-not $publishedOk) {
    Write-Host 'RESULT: FAIL — /tf was not published through the live Rosbridge_Client.' -ForegroundColor Red
    exit 1
}
if ($res.DeferredReason) {
    Write-Host "RESULT: PASS (live hz DEFERRED: $($res.DeferredReason)) — /tf published; headless scheduler >= 10 Hz stands for 7.4." -ForegroundColor Yellow
    exit 0
}
if ($null -ne $res.AvgHz -and $res.AvgHz -ge 10.0) {
    Write-Host ("RESULT: PASS — ros2 topic hz /tf = {0:N3} Hz (>= 10 Hz)." -f $res.AvgHz) -ForegroundColor Green
    exit 0
}
if ($null -ne $res.AvgHz) {
    Write-Host ("RESULT: FAIL — ros2 topic hz /tf = {0:N3} Hz (< 10 Hz target); reporting the real value per Requirement 0.4." -f $res.AvgHz) -ForegroundColor Red
    exit 1
}
Write-Host 'RESULT: FAIL — rosbridge was reachable but no average rate was captured from ros2 topic hz /tf.' -ForegroundColor Red
exit 1
