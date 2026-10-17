#Requires -Version 5.1
<#
.SYNOPSIS
    Task 18.1 [HOST] — end-to-end run (design.md "End-to-end run", Gate A).

.DESCRIPTION
    One command, non-interactive, one Unreal process at a time:
      1. Check the tracked tree is clean; record `git rev-parse HEAD`.
      2. Make sure rosbridge answers an HTTP 101 upgrade on localhost:9090 at
         url_path '//' (start it in a persistent WSL session if not).
      3. Build the editor target.
      4. Start `ros2 bag record -s sqlite3` in WSL for /lidar/points,
         /camera/image/compressed, /camera/camera_info, /tf into runs/<run_id>/bag.
      5. Launch RoverLevel with -game in waypoint mode and -E2ERun
         (ASensorRunManager: sensors, bridge, Run_Log), t.MaxFPS 120, vsync off.
      6. Once the Run_Log has records, run `ros2 topic hz` on /lidar/points and
         /camera/image/compressed for 60 s each, in parallel; raw output saved.
      7. The game saves 10 s of 10 fps viewport screenshots (game render only,
         never other desktop windows); FFmpeg turns them into a 640 px wide GIF at
         docs/media/demo.gif (must be < 10 MB).
      8. The game stops itself at >= MinCameraFrames frames AND >= MinDurationS s
         and quits cleanly (Run_Log flushed, run_done.txt with the average frame
         time); the bag is stopped with SIGINT.
      9. Point runs/latest at the run and run `analyze.py --run config/run.yaml`.
    Exit code 0 only when every step succeeded.

.EXAMPLE
    pwsh -NoProfile -File scripts\run_e2e.ps1
#>
[CmdletBinding()]
param(
    [string]$EnginePath = 'C:\Program Files (x86)\UE_5.8',
    [string]$WslDistro = 'Ubuntu',
    [int]$Port = 9090,
    [int]$Seed = 1234,
    [int]$MinCameraFrames = 2000,
    [int]$MinDurationS = 120,
    [int]$HzWindowSec = 60,
    [int]$RunTimeoutSec = 1800,
    [switch]$SkipBuild,
    [switch]$AllowDirty
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$UProject  = Join-Path $RepoRoot 'unreal\SensorSimSandbox\SensorSimSandbox.uproject'
$BuildBat  = Join-Path $EnginePath 'Engine\Build\BatchFiles\Build.bat'
$EditorExe = Join-Path $EnginePath 'Engine\Binaries\Win64\UnrealEditor.exe'
$Python    = Join-Path $RepoRoot '.venv\Scripts\python.exe'
$RunId     = 'e2e_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
$RunDir    = Join-Path $RepoRoot "runs\$RunId"
$HzDir     = Join-Path $RunDir 'hz'
$BagDir    = Join-Path $RunDir 'bag'
$LogDir    = Join-Path $RunDir 'logs'
$DemoDir   = Join-Path $RunDir 'demo_frames'
$GifPath   = Join-Path $RepoRoot 'docs\media\demo.gif'
New-Item -ItemType Directory -Force -Path $RunDir, $HzDir, $LogDir, (Split-Path $GifPath) | Out-Null

function Write-Step($m) { Write-Host "[run_e2e $(Get-Date -Format HH:mm:ss)] $m" }
function Fail($m) { Write-Host "RESULT: FAIL - $m"; exit 1 }

function Convert-ToWslPath([string]$WinPath) {
    $p = $WinPath -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') { return "/mnt/" + $Matches[1].ToLower() + $Matches[2] }
    return $p
}

function Invoke-Wsl([string]$BashCommand) {
    $out = & wsl.exe -d $WslDistro -- bash -lc "source /opt/ros/jazzy/setup.bash 2>/dev/null; $BashCommand" 2>&1 | Out-String
    return $out
}

# Run a bash script body in a detached, persistent WSL session (the session
# lives as long as the wsl.exe process, so background ROS processes survive).
function Start-WslDetached([string]$Name, [string]$Body) {
    $script = Join-Path $LogDir "$Name.sh"
    [IO.File]::WriteAllText($script, ("#!/usr/bin/env bash`nsource /opt/ros/jazzy/setup.bash`n$Body`n" -replace "`r`n", "`n"))
    return Start-Process -FilePath 'wsl.exe' -ArgumentList @('-d', $WslDistro, '--', 'bash', (Convert-ToWslPath $script)) `
        -PassThru -WindowStyle Hidden
}

function Test-WsUpgrade([int]$P = 9090, [string]$Path = '//') {
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect('127.0.0.1', $P)
        $stream = $client.GetStream()
        $key = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes((Get-Random).ToString()))
        $req = "GET $Path HTTP/1.1`r`nHost: 127.0.0.1:$P`r`nUpgrade: websocket`r`nConnection: Upgrade`r`nSec-WebSocket-Key: $key`r`nSec-WebSocket-Version: 13`r`n`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($req)
        $stream.Write($bytes, 0, $bytes.Length)
        Start-Sleep -Milliseconds 300
        $buf = New-Object byte[] 1024
        $n = $stream.Read($buf, 0, $buf.Length)
        $client.Close()
        return ((([Text.Encoding]::ASCII.GetString($buf, 0, $n)) -split "`r`n")[0] -match '101')
    } catch { return $false }
}

function Wait-ForNoUnrealProcess {
    for ($i = 0; $i -lt 60; $i++) {
        if (-not (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'UnrealBuildTool|UnrealEditor' })) { return }
        Start-Sleep -Seconds 2
    }
}

# --- 1. Clean tree + commit -------------------------------------------------
Set-Location $RepoRoot
$dirty = (& git status --porcelain --untracked-files=no) -join "`n"
if ($dirty -and -not $AllowDirty) { Fail "tracked tree is not clean:`n$dirty" }
$Commit = (& git rev-parse HEAD).Trim()
Write-Step "run_id=$RunId commit=$Commit seed=$Seed"

# --- 2. rosbridge -----------------------------------------------------------
$rosbridgeProc = $null
if (-not (Test-WsUpgrade -P $Port)) {
    Write-Step 'starting rosbridge (url_path=//) in a persistent WSL session'
    $null = Invoke-Wsl "pkill -9 -f rosbridge_websocket; pkill -9 -f rosapi_node; fuser -k $Port/tcp; sleep 1; true"
    $rosbridgeProc = Start-WslDetached 'rosbridge' "exec ros2 run rosbridge_server rosbridge_websocket --ros-args -p port:=$Port -p url_path:=//"
    $up = $false
    for ($i = 0; $i -lt 40 -and -not $up; $i++) { Start-Sleep -Seconds 1; $up = Test-WsUpgrade -P $Port }
    if (-not $up) { Fail "rosbridge did not answer HTTP 101 on localhost:$Port at '//'" }
}
Write-Step "rosbridge answers HTTP 101 on localhost:$Port at '//'"

# --- 3. Build ----------------------------------------------------------------
if (-not $SkipBuild) {
    Wait-ForNoUnrealProcess
    $compileLog = Join-Path $LogDir 'compile.log'
    Write-Step "building SensorSimSandboxEditor (log: $compileLog)"
    $p = Start-Process -FilePath $BuildBat -NoNewWindow -PassThru -RedirectStandardOutput $compileLog -RedirectStandardError "$compileLog.err" `
        -ArgumentList @('SensorSimSandboxEditor', 'Win64', 'Development', "-Project=`"$UProject`"", '-WaitMutex', '-NoHotReloadFromIDE')
    if (-not $p.WaitForExit(1800 * 1000)) { try { $p.Kill() } catch { }; Fail 'build timed out' }
    if (-not ((Get-Content $compileLog -Raw) -match '(?m)^\s*Result:\s*Succeeded')) { Fail "build failed (see $compileLog)" }
}

# --- 4. Bag recording ---------------------------------------------------------
$topics = '/lidar/points /camera/image/compressed /camera/camera_info /tf /imu /gps/fix'
$bagProc = Start-WslDetached 'bag' "exec ros2 bag record -s sqlite3 -o '$(Convert-ToWslPath $BagDir)' $topics > '$(Convert-ToWslPath (Join-Path $LogDir 'bag.log'))' 2>&1"
Start-Sleep -Seconds 5
Write-Step "bag recording started -> $BagDir"

# --- 5. Launch the game -----------------------------------------------------
Wait-ForNoUnrealProcess
$editorLog = Join-Path $LogDir 'game.log'
$runLog = Join-Path $RunDir 'run_log.jsonl'
$gameArgs = @(
    "`"$UProject`"", '/Game/Maps/RoverLevel', '-game', '-windowed', '-ResX=1280', '-ResY=720',
    '-E2ERun', '-RoverMode=Waypoint', "-RunDir=`"$RunDir`"", "-Seed=$Seed",
    "-MinCameraFrames=$MinCameraFrames", "-MinDurationS=$MinDurationS", "-DemoFramesDir=`"$DemoDir`"",
    '-BridgeHost=127.0.0.1', "-BridgePort=$Port",
    '-ExecCmds="t.MaxFPS 120,r.VSync 0"',
    '-unattended', '-nopause', '-nosplash', '-log', "-abslog=`"$editorLog`""
)
Write-Step "launching -game RoverLevel -E2ERun (log: $editorLog)"
$game = Start-Process -FilePath $EditorExe -ArgumentList $gameArgs -PassThru
$started = Get-Date

# Wait for records to flow.
$flowing = $false
for ($i = 0; $i -lt 300 -and -not $flowing -and -not $game.HasExited; $i++) {
    Start-Sleep -Seconds 2
    $flowing = (Test-Path $runLog) -and ((Get-Item $runLog).Length -gt 100000)
}
if (-not $flowing) { Fail "no Run_Log records after launch (game exited=$($game.HasExited); see $editorLog)" }
Write-Step 'Run_Log records flowing; starting ros2 topic hz windows'
Start-Sleep -Seconds 5

# --- 6. ros2 topic hz, 60 s each, in parallel --------------------------------
$hzProcs = @()
foreach ($t in @('/lidar/points', '/camera/image/compressed')) {
    $f = Convert-ToWslPath (Join-Path $HzDir (($t.Trim('/') -replace '/', '_') + '.txt'))
    $name = 'hz_' + ($t.Trim('/') -replace '/', '_')
    $hzProcs += Start-WslDetached $name "timeout -s INT $HzWindowSec stdbuf -oL ros2 topic hz $t > '$f' 2>&1; echo done >> '$f'"
}
# One message each from the M5 topics (Requirements 11.1, 11.2).
foreach ($t in @('/imu', '/gps/fix')) {
    $f = Convert-ToWslPath (Join-Path $LogDir ('echo_' + ($t.Trim('/') -replace '/', '_') + '.txt'))
    $hzProcs += Start-WslDetached ('echo_' + ($t.Trim('/') -replace '/', '_')) "timeout 30 ros2 topic echo --once $t > '$f' 2>&1; true"
}

foreach ($h in $hzProcs) { $null = $h.WaitForExit(($HzWindowSec + 30) * 1000) }
Write-Step 'ros2 topic hz windows finished'

# --- 8. Wait for the game to finish and stop the bag -------------------------
$remaining = [Math]::Max(60, $RunTimeoutSec - [int]((Get-Date) - $started).TotalSeconds)
if (-not $game.WaitForExit($remaining * 1000)) {
    try { $game.CloseMainWindow() | Out-Null; Start-Sleep 10; if (-not $game.HasExited) { $game.Kill() } } catch { }
    Fail "game did not finish within $RunTimeoutSec s"
}
Start-Sleep -Seconds 3
$null = Invoke-Wsl "pkill -INT -f 'ros2 bag record'; sleep 5; true"
$null = $bagProc.WaitForExit(30000)
$doneFile = Join-Path $RunDir 'run_done.txt'
$done = if (Test-Path $doneFile) { Get-Content $doneFile -Raw } else { '' }
Write-Step "run_done.txt:`n$done"

# --- 7. Demo GIF from the game's own viewport screenshots --------------------
$gifOk = $false
$frames = @(Get-ChildItem -Path $DemoDir -Filter 'demo_*.png' -ErrorAction SilentlyContinue)
if ($frames.Count -ge 50) {
    $palette = Join-Path $LogDir 'palette.png'
    $pattern = Join-Path $DemoDir 'demo_%04d.png'
    & ffmpeg -hide_banner -loglevel error -y -framerate 10 -i $pattern -vf 'scale=640:-1:flags=lanczos,palettegen' $palette
    & ffmpeg -hide_banner -loglevel error -y -framerate 10 -i $pattern -i $palette -lavfi 'scale=640:-1:flags=lanczos[x];[x][1:v]paletteuse' $GifPath
    $gifOk = (Test-Path $GifPath) -and ((Get-Item $GifPath).Length -gt 0) -and ((Get-Item $GifPath).Length -lt 10MB)
}
Write-Step ("demo GIF: ok={0} frames={1} size={2:N0} bytes" -f $gifOk, $frames.Count, $(if (Test-Path $GifPath) { (Get-Item $GifPath).Length } else { 0 }))
if ($done -notmatch 'status=PASS') { Fail "run did not complete (see $doneFile and $editorLog)" }

# --- 9. Analysis -------------------------------------------------------------
$latest = Join-Path $RepoRoot 'runs\latest'
if (Test-Path $latest) { (Get-Item $latest).Delete() }
New-Item -ItemType Junction -Path $latest -Target $RunDir | Out-Null
Write-Step "runs\latest -> $RunDir; running analysis"
& $Python (Join-Path $RepoRoot 'analysis\analyze.py') --run config/run.yaml
$analysisExit = $LASTEXITCODE
if ($rosbridgeProc) { Write-Step 'rosbridge left running for follow-up checks' }
if ($analysisExit -ne 0) { Fail "analyze.py exited $analysisExit" }
if (-not $gifOk) { Fail 'demo GIF missing or >= 10 MB' }
Write-Host "RESULT: PASS - run_id=$RunId commit=$Commit seed=$Seed"
exit 0
