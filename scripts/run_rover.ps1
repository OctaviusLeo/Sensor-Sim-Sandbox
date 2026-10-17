#Requires -Version 5.1
<#
.SYNOPSIS
    Task 4.1 demo/verify for the rover, waypoint controller, and RoverLevel.

.DESCRIPTION
    Three steps, all non-interactive and host-run (one Unreal process at a time,
    long steps polled in the background):

      1. Build the editor target with Build.bat ... -WaitMutex (so no "rebuild
         modules" prompt can appear on a later launch), compile log saved.
      2. Headless state-machine check: run the `RoverSelfCheck` console command
         via UnrealEditor-Cmd with -nullrhi (no window). This drives the PURE
         waypoint state machine (order, advance-within-tolerance, final hold,
         timeout + unreached report) and validates config/level.yaml content.
         Passes only when the log contains "RoverSelfCheck PASS".
      3. Build the minimal /Game/Maps/RoverLevel package headlessly (editor
         Python), then do a short `-game` launch that loads RoverLevel in
         waypoint mode with config/level.yaml; the launch log is saved as
         evidence that the level loads and the configured waypoints are driven.

    Each step writes its log under runs/logs/rover/ (gitignored). The script
    exits 0 only when the build compiles and the headless self-check passes; the
    -game launch is best-effort evidence on a machine without a GPU and is
    reported but does not alone fail the gate (its blocker, if any, is recorded).

.PARAMETER EnginePath
    UE 5.8 install root. Default is the verified path on this host.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_rover.ps1
#>

[CmdletBinding()]
param(
    [string]$EnginePath = 'C:\Program Files (x86)\UE_5.8',
    [int]$SelfCheckTimeoutSec = 1800,
    [int]$GameLaunchSeconds = 60,
    [switch]$SkipGameLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$UProject   = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$LevelCfg   = Join-Path $RepoRoot 'config\level.yaml'
$LogDir     = Join-Path $RepoRoot 'runs\logs\rover'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$BuildBat     = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorCmdExe = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$EditorExe    = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor.exe'

function Write-Step($msg) { Write-Host "[run_rover] $msg" }

# --- 1. Build the editor target -------------------------------------------
function Invoke-Build {
    $compileLog = Join-Path $LogDir 'compile.log'
    if (-not (Test-Path $BuildBat)) {
        "Build.bat not found at $BuildBat" | Set-Content -Path $compileLog -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = "Build.bat not found at $BuildBat"; Log = $compileLog }
    }
    Write-Step "Building SensorSimSandboxEditor (log: $compileLog) ..."
    $args = @(
        'SensorSimSandboxEditor', 'Win64', 'Development',
        "-Project=`"$UProject`"", '-WaitMutex', '-NoHotReloadFromIDE'
    )
    $p = Start-Process -FilePath $BuildBat -ArgumentList $args -NoNewWindow -PassThru `
        -RedirectStandardOutput $compileLog -RedirectStandardError "$compileLog.err"
    $p.WaitForExit()
    $ok = ($p.ExitCode -eq 0)
    return [pscustomobject]@{ Ok = $ok; Detail = "Build.bat exit $($p.ExitCode)"; Log = $compileLog }
}

# --- 2. Headless state-machine self-check ---------------------------------
function Invoke-SelfCheck {
    $log = Join-Path $LogDir 'selfcheck.log'
    if (-not (Test-Path $EditorCmdExe)) {
        "UnrealEditor-Cmd.exe not found at $EditorCmdExe" | Set-Content -Path $log -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = 'editor-cmd missing'; Log = $log }
    }
    Write-Step "Running headless RoverSelfCheck (log: $log) ..."
    $execCmds = "RoverSelfCheck `"$LevelCfg`";Quit"
    $args = @(
        "`"$UProject`"",
        "-ExecCmds=`"$execCmds`"",
        '-unattended', '-nopause', '-nosplash', '-nullrhi',
        "-abslog=`"$log`""
    )
    $p = Start-Process -FilePath $EditorCmdExe -ArgumentList $args -NoNewWindow -PassThru
    if (-not $p.WaitForExit($SelfCheckTimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        return [pscustomobject]@{ Ok = $false; Detail = 'self-check timed out'; Log = $log }
    }
    $passed = $false
    if (Test-Path $log) {
        $passed = Select-String -Path $log -Pattern 'RoverSelfCheck PASS' -Quiet
    }
    return [pscustomobject]@{ Ok = $passed; Detail = "RoverSelfCheck PASS found: $passed"; Log = $log }
}

# --- 3. Build level package + short -game launch --------------------------
function Invoke-BuildLevel {
    $log = Join-Path $LogDir 'build_level.log'
    if (-not (Test-Path $EditorCmdExe)) { return [pscustomobject]@{ Ok=$false; Detail='editor-cmd missing'; Log=$log } }
    Write-Step "Creating /Game/Maps/RoverLevel package (log: $log) ..."
    $script = Join-Path $PSScriptRoot 'build_rover_level.py'
    $args = @(
        "`"$UProject`"", '-run=pythonscript', "-script=`"$script`"",
        '-unattended', '-nopause', '-nosplash', "-abslog=`"$log`""
    )
    $p = Start-Process -FilePath $EditorCmdExe -ArgumentList $args -NoNewWindow -PassThru
    $null = $p.WaitForExit($SelfCheckTimeoutSec * 1000)
    return [pscustomobject]@{ Ok = ($p.ExitCode -eq 0); Detail = "pythonscript exit $($p.ExitCode)"; Log = $log }
}

function Invoke-GameLaunch {
    $log = Join-Path $LogDir 'game_launch.log'
    if (-not (Test-Path $EditorExe)) { return [pscustomobject]@{ Ok=$false; Detail='UnrealEditor.exe missing'; Log=$log } }
    $runDir = Join-Path $RepoRoot ("runs\rover_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    Write-Step "Short -game launch of RoverLevel in waypoint mode (log: $log) ..."
    $args = @(
        "`"$UProject`"", '/Game/Maps/RoverLevel', '-game', '-windowed',
        '-ResX=1280', '-ResY=720',
        '-RoverMode=Waypoint', "-RoverConfig=`"$LevelCfg`"", "-RunDir=`"$runDir`"",
        "-abslog=`"$log`""
    )
    $p = Start-Process -FilePath $EditorExe -ArgumentList $args -PassThru
    Start-Sleep -Seconds $GameLaunchSeconds
    if (-not $p.HasExited) { try { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; if (-not $p.HasExited) { $p.Kill() } } catch {} }
    $loaded = $false
    if (Test-Path $log) {
        $loaded = Select-String -Path $log -Pattern 'RoverLevelBuilder building from|Spawned ARover' -Quiet
    }
    return [pscustomobject]@{ Ok = $loaded; Detail = "level load evidence: $loaded"; Log = $log }
}

# --- Orchestration ---------------------------------------------------------
$build = Invoke-Build
Write-Step "Build: Ok=$($build.Ok) $($build.Detail)"
if (-not $build.Ok) { Write-Error "Build failed; see $($build.Log)"; exit 1 }

$check = Invoke-SelfCheck
Write-Step "SelfCheck: Ok=$($check.Ok) $($check.Detail)"
if (-not $check.Ok) { Write-Error "Headless self-check failed; see $($check.Log)"; exit 1 }

if (-not $SkipGameLaunch) {
    $lvl = Invoke-BuildLevel
    Write-Step "BuildLevel: Ok=$($lvl.Ok) $($lvl.Detail)"
    $game = Invoke-GameLaunch
    Write-Step "GameLaunch: Ok=$($game.Ok) $($game.Detail)"
    if (-not $game.Ok) {
        Write-Step "NOTE: -game launch did not produce load evidence; record as a host blocker if the GPU/engine is unavailable. The headless state-machine check (the gate) passed."
    }
}

Write-Step 'Done. Build + headless waypoint state-machine self-check passed.'
exit 0
