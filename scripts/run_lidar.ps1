#Requires -Version 5.1
<#
.SYNOPSIS
    Task 6.1 demo/verify for the Lidar_Model (raycast scan + shared pipeline).

.DESCRIPTION
    Two steps, non-interactive and host-run (one Unreal process at a time, long
    steps polled; the UBT Makefile cache is cleared first so the new Sensors/*
    source files are picked up):

      1. Build the editor target with Build.bat ... -WaitMutex (so no "rebuild
         modules" prompt can appear on a later launch); compile log saved.
      2. Headless scan: launch /Game/Maps/RoverLevel with -game (a live world is
         required for line traces) and -ExecCmds="LidarSelfCheck ... ; Quit".
         LidarSelfCheck loads config/lidar.yaml (Gate A values), spawns an
         enclosing scene with collision, performs raycast scans capturing per-ray
         Ground_Truth BEFORE the pipeline, and logs >= 1000 returns each with its
         truth and published value. It also proves no-hit rays are null/excluded
         and that ranges are not clamped. Passes only when the log contains
         "LidarSelfCheck PASS".

    The launch log is saved under runs/logs/lidar/ (gitignored) and copied to
    docs/evidence/ as the task's quoted evidence.

.PARAMETER EnginePath
    UE 5.8 install root. Default is the verified path on this host.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_lidar.ps1
#>

[CmdletBinding()]
param(
    [string]$EnginePath = 'C:\Program Files (x86)\UE_5.8',
    [int]$BuildTimeoutSec = 1800,
    [int]$ScanTimeoutSec = 900,
    [int]$MinReturns = 1000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$UProject   = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$LidarCfg   = Join-Path $RepoRoot 'config\lidar.yaml'
$LogDir     = Join-Path $RepoRoot 'runs\logs\lidar'
$EvidenceDir= Join-Path $RepoRoot 'docs\evidence'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null

$BuildBat     = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorExe    = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor.exe'

function Write-Step($msg) { Write-Host "[run_lidar] $msg" }

# --- 0. Clear the UBT Makefile cache so new source files are picked up -----
function Clear-UbtCache {
    $intermediate = Join-Path $RepoRoot 'unreal\SensorSimSandbox\Intermediate\Build'
    if (Test-Path $intermediate) {
        Get-ChildItem -Path $intermediate -Recurse -Filter 'Makefile.bin' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
        Write-Step "cleared UBT Makefile.bin cache under $intermediate"
    }
}

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
    if (-not $p.WaitForExit($BuildTimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        return [pscustomobject]@{ Ok = $false; Detail = 'build timed out'; Log = $compileLog }
    }
    $ok = ($p.ExitCode -eq 0)
    return [pscustomobject]@{ Ok = $ok; Detail = "Build.bat exit $($p.ExitCode)"; Log = $compileLog }
}

# --- 2. Headless raycast scan (needs a live world) ------------------------
function Invoke-Scan {
    $log = Join-Path $LogDir 'lidar_selfcheck.log'
    if (-not (Test-Path $EditorExe)) {
        "UnrealEditor.exe not found at $EditorExe" | Set-Content -Path $log -Encoding UTF8
        return [pscustomobject]@{ Ok = $false; Detail = 'editor missing'; Log = $log }
    }
    Write-Step "Running headless LidarSelfCheck via -game RoverLevel (log: $log) ..."
    # LidarSelfCheck needs a world for line traces, so launch a map with -game.
    # Give the world a moment to come up, then run the check and quit.
    $execCmds = "LidarSelfCheck `"$LidarCfg`" $MinReturns;Quit"
    $args = @(
        "`"$UProject`"", '/Game/Maps/RoverLevel', '-game',
        '-ResX=640', '-ResY=480', '-windowed',
        "-ExecCmds=`"$execCmds`"",
        '-unattended', '-nopause', '-nosplash',
        "-abslog=`"$log`""
    )
    $p = Start-Process -FilePath $EditorExe -ArgumentList $args -PassThru
    if (-not $p.WaitForExit($ScanTimeoutSec * 1000)) {
        try { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep 3; if (-not $p.HasExited) { $p.Kill() } } } catch {}
    }
    $passed = $false
    if (Test-Path $log) {
        $passed = Select-String -Path $log -Pattern 'LidarSelfCheck PASS' -Quiet
    }
    # Copy the log into docs/evidence as the task's quoted evidence.
    if (Test-Path $log) {
        Copy-Item $log (Join-Path $EvidenceDir 'task_6_1_lidar_selfcheck.log') -Force
    }
    return [pscustomobject]@{ Ok = $passed; Detail = "LidarSelfCheck PASS found: $passed"; Log = $log }
}

# --- Orchestration ---------------------------------------------------------
Clear-UbtCache

$build = Invoke-Build
Write-Step "Build: Ok=$($build.Ok) $($build.Detail)"
if (-not $build.Ok) { Write-Error "Build failed; see $($build.Log)"; exit 1 }

$scan = Invoke-Scan
Write-Step "Scan: Ok=$($scan.Ok) $($scan.Detail)"
if (-not $scan.Ok) { Write-Error "Headless lidar scan did not pass; see $($scan.Log)"; exit 1 }

Write-Step 'Done. Build + headless lidar scan (>=1000 returns, truth+published) passed.'
exit 0
